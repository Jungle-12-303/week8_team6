#include "db_api.h"

/*
 * ============================================================================
 * [DbApi 코드 리뷰용 흐름 지도]
 * ============================================================================
 *
 * 이 파일의 역할:
 * - HTTP API 서버와 기존 week7 SQL 엔진 사이의 adapter 역할을 한다.
 * - 기존 엔진은 FILE* 로 결과를 출력하므로, API 응답용 문자열로 다시 읽어온다.
 * - 여러 worker thread 가 동시에 들어와도 DB 엔진은 mutex 로 한 번에 하나씩 실행한다.
 *
 * 전체 호출 흐름:
 *
 * handle_query()                         -- api_server.c
 *      |
 *      v
 * db_api_execute_sql()
 *      |
 *      +--> tmpfile()                    -- 엔진 출력 캡처용 임시 FILE*
 *      |
 *      +--> mutex_lock()
 *      |       기존 SQL 엔진/CSV 파일/B+ tree 인덱스 공유 보호
 *      |
 *      +--> sql_engine_execute_sql()
 *      |       SELECT/INSERT 파싱 및 database.c 실행
 *      |
 *      +--> read_stream_to_string()
 *      |       FILE* 결과를 JSON 변환 가능한 char* 로 복사
 *      |
 *      +--> mutex_unlock()
 *      |
 *      v
 * DbApiResult 반환
 *
 * 왜 mutex 가 필요한가:
 * thread pool 은 요청을 병렬로 받지만, 기존 저장소 코드는 같은 users.tbl/users.idx
 * 파일과 SqlEngine 메타데이터를 공유한다. 동시에 INSERT/SELECT 가 섞이면 파일 offset,
 * B+ tree 상태가 꼬일 수 있어 DB 엔진 실행 구간만 직렬화한다.
 * ============================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <windows.h>
typedef CRITICAL_SECTION DbApiMutex;

static int mutex_init(DbApiMutex *mutex) {
    InitializeCriticalSection(mutex);
    return 1;
}

static void mutex_destroy(DbApiMutex *mutex) {
    DeleteCriticalSection(mutex);
}

static void mutex_lock(DbApiMutex *mutex) {
    EnterCriticalSection(mutex);
}

static void mutex_unlock(DbApiMutex *mutex) {
    LeaveCriticalSection(mutex);
}
#else
#include <pthread.h>
typedef pthread_mutex_t DbApiMutex;

static int mutex_init(DbApiMutex *mutex) {
    return pthread_mutex_init(mutex, NULL) == 0;
}

static void mutex_destroy(DbApiMutex *mutex) {
    pthread_mutex_destroy(mutex);
}

static void mutex_lock(DbApiMutex *mutex) {
    pthread_mutex_lock(mutex);
}

static void mutex_unlock(DbApiMutex *mutex) {
    pthread_mutex_unlock(mutex);
}
#endif

/*
 * tmpfile 에 쌓인 엔진 출력 전체를 heap 문자열로 읽어온다.
 *
 * 단계:
 * 1. fflush 로 엔진이 쓴 내용을 파일에 반영
 * 2. fseek/ftell 로 출력 길이 계산
 * 3. 처음으로 되감은 뒤 fread 로 전체 복사
 */
static char *read_stream_to_string(FILE *file, char *error_buf, size_t error_buf_size) {
    long length;
    size_t read_size;
    char *buffer;

    /*
     * The old engine writes SELECT/INSERT output to FILE*.
     * The API server needs that same output as a heap string for JSON.
     */
    if (fflush(file) != 0 || fseek(file, 0, SEEK_END) != 0) {
        snprintf(error_buf, error_buf_size, "failed to seek query result stream");
        return NULL;
    }

    length = ftell(file);
    if (length < 0) {
        snprintf(error_buf, error_buf_size, "failed to read query result length");
        return NULL;
    }

    if (fseek(file, 0, SEEK_SET) != 0) {
        snprintf(error_buf, error_buf_size, "failed to rewind query result stream");
        return NULL;
    }

    buffer = (char *)malloc((size_t)length + 1);
    if (buffer == NULL) {
        snprintf(error_buf, error_buf_size, "failed to allocate query result buffer");
        return NULL;
    }

    read_size = fread(buffer, 1, (size_t)length, file);
    buffer[read_size] = '\0';
    return buffer;
}

/*
 * API 서버 시작 시 한 번 호출된다.
 * DbApi 구조체 안에 기존 SqlEngine 과 mutex 를 함께 준비한다.
 */
int db_api_init(DbApi *api, const char *data_dir, char *error_buf, size_t error_buf_size) {
    DbApiMutex *mutex;

    memset(api, 0, sizeof(*api));

    /*
     * Keep the mutex opaque in db_api.h so public headers do not expose
     * windows.h or pthread.h to every file that includes DbApi.
     */
    mutex = (DbApiMutex *)malloc(sizeof(DbApiMutex));
    if (mutex == NULL) {
        snprintf(error_buf, error_buf_size, "failed to allocate DB mutex");
        return 0;
    }

    if (!mutex_init(mutex)) {
        free(mutex);
        snprintf(error_buf, error_buf_size, "failed to initialize DB mutex");
        return 0;
    }

    api->mutex_impl = mutex;
    if (!sql_engine_init(&api->engine, data_dir, error_buf, error_buf_size)) {
        mutex_destroy(mutex);
        free(mutex);
        api->mutex_impl = NULL;
        return 0;
    }

    return 1;
}

/*
 * 서버 종료 시 DB 엔진과 mutex 를 정리한다.
 */
void db_api_free(DbApi *api) {
    if (api == NULL) {
        return;
    }

    sql_engine_free(&api->engine);
    if (api->mutex_impl != NULL) {
        DbApiMutex *mutex = (DbApiMutex *)api->mutex_impl;
        mutex_destroy(mutex);
        free(mutex);
        api->mutex_impl = NULL;
    }
}

/*
 * HTTP worker 가 SQL 하나를 실행할 때 호출하는 함수.
 *
 * 성공:
 * - result->ok = 1
 * - result->output = 엔진이 출력한 CSV/메시지 문자열
 * - result->stats = 인덱스 사용 여부와 row 통계
 *
 * 실패:
 * - result->ok = 0
 * - result->error 에 파싱/실행 실패 이유 저장
 */
int db_api_execute_sql(DbApi *api, const char *sql_text, DbApiResult *result) {
    FILE *output;
    char capture_error[512];

    memset(result, 0, sizeof(*result));
    memset(capture_error, 0, sizeof(capture_error));

    output = tmpfile();
    if (output == NULL) {
        snprintf(result->error, sizeof(result->error), "failed to create a temporary result stream");
        return 0;
    }

    /*
     * Critical section:
     * - Database files are shared.
     * - B+ tree index files are shared.
     * - table metadata inside SqlEngine is shared.
     * So only one SQL statement may execute against the old engine at a time.
     */
    mutex_lock((DbApiMutex *)api->mutex_impl);
    result->ok = sql_engine_execute_sql(&api->engine,
                                        sql_text,
                                        output,
                                        &result->stats,
                                        result->error,
                                        sizeof(result->error));
    if (result->ok) {
        result->output = read_stream_to_string(output, capture_error, sizeof(capture_error));
        if (result->output == NULL) {
            result->ok = 0;
            snprintf(result->error, sizeof(result->error), "%s", capture_error);
        }
    }
    mutex_unlock((DbApiMutex *)api->mutex_impl);

    fclose(output);

    if (!result->ok) {
        db_api_result_free(result);
        return 0;
    }

    if (result->output == NULL) {
        result->output = (char *)malloc(1);
        if (result->output == NULL) {
            snprintf(result->error, sizeof(result->error), "failed to allocate empty result buffer");
            return 0;
        }
        result->output[0] = '\0';
    }

    return 1;
}

/*
 * DbApiResult 안의 output 은 heap 문자열이다.
 * format_query_body() 로 응답을 만든 뒤 반드시 해제해야 한다.
 */
void db_api_result_free(DbApiResult *result) {
    if (result == NULL) {
        return;
    }

    free(result->output);
    result->output = NULL;
}
