#include "db_api.h"

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

void db_api_result_free(DbApiResult *result) {
    if (result == NULL) {
        return;
    }

    free(result->output);
    result->output = NULL;
}
