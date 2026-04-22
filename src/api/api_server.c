#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200112L
#endif

/*
 * ============================================================================
 * [Mini DBMS API Server 코드 리뷰용 흐름 지도]
 * ============================================================================
 *
 * 이 파일의 역할:
 * - TCP 소켓으로 HTTP 요청을 받는다.
 * - /health, /query, /metrics 라우트를 구분한다.
 * - /query 요청이면 SQL 문자열을 뽑아 DbApi 로 전달한다.
 * - 기존 week7 SQL 엔진의 CSV 결과를 Thunder Client 에서 보기 쉬운 JSON 으로 감싼다.
 *
 * 전체 호출 흐름:
 *
 * [Thunder Client]
 *      |
 *      v
 * server_main.c:main()
 *      |  서버 설정(host/port/data-dir/thread 수)을 만든다.
 *      v
 * api_server_run()
 *      |  DB 엔진 초기화, thread pool 생성, listen socket 준비
 *      v
 * open_listener()
 *      |  bind/listen 으로 클라이언트 접속 대기 상태를 만든다.
 *      v
 * accept() loop
 *      |  클라이언트 연결 1개당 ClientTask 생성
 *      v
 * thread_pool_submit(..., handle_client, task)
 *      |  실제 HTTP 처리 작업을 worker thread 에게 맡긴다.
 *      v
 * handle_client()
 *      |
 *      +--> read_http_request()
 *      |       HTTP request line, header, body 를 읽는다.
 *      |
 *      +--> [라우트 분기]
 *              |
 *              +-- OPTIONS        -> CORS 사전 요청 응답
 *              +-- GET /health    -> 서버 준비 상태 응답
 *              +-- GET /metrics   -> 누적 요청 통계 응답
 *              +-- POST /query    -> handle_query()
 *              +-- 그 외          -> 404 JSON 에러
 *
 * POST /query 세부 흐름:
 *
 * handle_query()
 *      |
 *      +--> extract_sql_text()
 *      |       body 가 {"sql":"..."} 이면 JSON 에서 SQL 추출
 *      |       body 가 raw text 이면 앞뒤 공백 제거 후 그대로 SQL 사용
 *      |
 *      +--> db_api_execute_sql()
 *      |       기존 SQL 엔진을 호출한다. 자세한 mutex/캡처 흐름은 db_api.c 참고.
 *      |
 *      +--> format_query_body()
 *              성공: {"ok":true, "result":"CSV...", "columns":[], "rows":[], "stats":...}
 *              실패: {"ok":false, "error":"..."}
 *
 * 중요한 설계 선택:
 * - 클라이언트 연결 처리는 thread pool 로 병렬 처리한다.
 * - 기존 DB 엔진 자체는 thread-safe 하게 만들어진 코드가 아니므로 DbApi 내부 mutex 로 직렬화한다.
 * - HTTP keep-alive 는 구현하지 않고 요청 1개 처리 후 연결을 닫는다. 과제 시연에는 이 방식이 단순하고 충분하다.
 * ============================================================================
 */

#include "api_server.h"

#include "db_api.h"
#include "thread_pool.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET ApiSocket;
#define API_INVALID_SOCKET INVALID_SOCKET
#define API_CLOSE_SOCKET closesocket
#define API_SOCKET_ERROR_TEXT() WSAGetLastError()
#else
#include <errno.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
typedef int ApiSocket;
#define API_INVALID_SOCKET (-1)
#define API_CLOSE_SOCKET close
#define API_SOCKET_ERROR_TEXT() errno
#endif

#define HEADER_LIMIT_BYTES 8192

typedef struct {
    char method[8];
    char path[128];
    char *body;
    size_t body_len;
} HttpRequest;

typedef struct {
    size_t total_requests;
    size_t query_requests;
    size_t ok_responses;
    size_t error_responses;
} ApiMetrics;

/*
 * Metrics use their own lock because many worker threads update counters.
 * The DB has a separate lock inside DbApi, so metric updates never block SQL
 * execution longer than needed.
 */
#if defined(_WIN32)
typedef CRITICAL_SECTION MetricsMutex;

static int metrics_mutex_init(MetricsMutex *mutex) {
    InitializeCriticalSection(mutex);
    return 1;
}

static void metrics_mutex_destroy(MetricsMutex *mutex) {
    DeleteCriticalSection(mutex);
}

static void metrics_mutex_lock(MetricsMutex *mutex) {
    EnterCriticalSection(mutex);
}

static void metrics_mutex_unlock(MetricsMutex *mutex) {
    LeaveCriticalSection(mutex);
}
#else
#include <pthread.h>
typedef pthread_mutex_t MetricsMutex;

static int metrics_mutex_init(MetricsMutex *mutex) {
    return pthread_mutex_init(mutex, NULL) == 0;
}

static void metrics_mutex_destroy(MetricsMutex *mutex) {
    pthread_mutex_destroy(mutex);
}

static void metrics_mutex_lock(MetricsMutex *mutex) {
    pthread_mutex_lock(mutex);
}

static void metrics_mutex_unlock(MetricsMutex *mutex) {
    pthread_mutex_unlock(mutex);
}
#endif

typedef struct {
    DbApi db;
    ThreadPool *pool;
    size_t max_body_bytes;
    ApiMetrics metrics;
    MetricsMutex metrics_mutex;
} ApiServer;

typedef struct {
    ApiServer *server;
    ApiSocket client;
} ClientTask;

/*
 * 문자열의 일부만 heap 문자열로 복사한다.
 * HTTP body, CSV field, JSON 문자열 조립처럼 "원본 버퍼의 일부"를
 * 독립적으로 보관해야 할 때 사용한다.
 */
static char *duplicate_range(const char *text, size_t length) {
    char *copy = (char *)malloc(length + 1);

    if (copy == NULL) {
        return NULL;
    }

    memcpy(copy, text, length);
    copy[length] = '\0';
    return copy;
}

/*
 * HTTP 헤더 이름은 대소문자를 구분하지 않는다.
 * 그래서 Content-Length 와 content-length 를 같은 값으로 보기 위해 사용한다.
 */
static int starts_with_ci(const char *text, const char *prefix, size_t prefix_len) {
    size_t index;

    for (index = 0; index < prefix_len; ++index) {
        unsigned char left = (unsigned char)text[index];
        unsigned char right = (unsigned char)prefix[index];
        if (tolower(left) != tolower(right)) {
            return 0;
        }
    }

    return 1;
}

/*
 * read_http_request() 가 body 를 heap 에 만들기 때문에 요청 처리가 끝나면
 * 반드시 이 함수로 정리한다.
 */
static void request_free(HttpRequest *request) {
    free(request->body);
    request->body = NULL;
    request->body_len = 0;
}

/*
 * HTTP header 와 body 의 경계는 빈 줄이다.
 * 보통 "\r\n\r\n" 이지만, 단순 테스트 클라이언트를 위해 "\n\n" 도 허용한다.
 */
static size_t find_header_end(const char *buffer, size_t length) {
    size_t index;

    for (index = 0; index + 3 < length; ++index) {
        if (buffer[index] == '\r' &&
            buffer[index + 1] == '\n' &&
            buffer[index + 2] == '\r' &&
            buffer[index + 3] == '\n') {
            return index + 4;
        }
    }

    for (index = 0; index + 1 < length; ++index) {
        if (buffer[index] == '\n' && buffer[index + 1] == '\n') {
            return index + 2;
        }
    }

    return 0;
}

/*
 * Content-Length 는 body 를 몇 byte 더 읽어야 하는지 알려준다.
 * POST /query 에서 SQL 문자열이 body 에 들어오므로 이 값이 필요하다.
 */
static int parse_content_length(const char *headers, size_t header_len, size_t *content_length) {
    const char name[] = "Content-Length:";
    size_t name_len = sizeof(name) - 1;
    size_t line_start = 0;

    *content_length = 0;

    while (line_start < header_len) {
        size_t line_end = line_start;
        size_t value_index;
        size_t parsed = 0;
        int saw_digit = 0;

        while (line_end < header_len && headers[line_end] != '\n' && headers[line_end] != '\r') {
            ++line_end;
        }

        if (line_end - line_start > name_len &&
            starts_with_ci(headers + line_start, name, name_len)) {
            value_index = line_start + name_len;
            while (value_index < line_end && isspace((unsigned char)headers[value_index])) {
                ++value_index;
            }

            while (value_index < line_end && isdigit((unsigned char)headers[value_index])) {
                parsed = parsed * 10 + (size_t)(headers[value_index] - '0');
                saw_digit = 1;
                ++value_index;
            }

            while (value_index < line_end && isspace((unsigned char)headers[value_index])) {
                ++value_index;
            }

            if (!saw_digit || value_index != line_end) {
                return 0;
            }

            *content_length = parsed;
            return 1;
        }

        while (line_end < header_len && (headers[line_end] == '\n' || headers[line_end] == '\r')) {
            ++line_end;
        }
        line_start = line_end;
    }

    return 1;
}

/*
 * 클라이언트 소켓에서 HTTP 요청 1개를 끝까지 읽는다.
 *
 * 처리 단계:
 * 1. header 끝(\r\n\r\n)을 찾을 때까지 recv() 반복
 * 2. request line 에서 method/path 추출
 * 3. Content-Length 만큼 body 를 추가로 읽음
 * 4. body 만 request->body 로 복사
 *
 * 왜 필요한가:
 * TCP 는 "메시지 단위"가 아니라 byte stream 이다. recv() 한 번에 요청 전체가
 * 온다는 보장이 없으므로 header/body 길이를 직접 확인하며 모아야 한다.
 */
static int read_http_request(ApiSocket client,
                             size_t max_body_bytes,
                             HttpRequest *request,
                             char *error_buf,
                             size_t error_buf_size) {
    char *buffer;
    size_t capacity = HEADER_LIMIT_BYTES + max_body_bytes + 1;
    size_t total = 0;
    size_t header_end = 0;
    size_t content_length = 0;

    memset(request, 0, sizeof(*request));

    buffer = (char *)malloc(capacity);
    if (buffer == NULL) {
        snprintf(error_buf, error_buf_size, "failed to allocate HTTP request buffer");
        return 0;
    }

    /*
     * A worker reads exactly one HTTP request. Thunder Client sends simple
     * request/response calls, so HTTP keep-alive is intentionally not needed.
     */
    while (header_end == 0) {
        int received;
        int chunk;

        if (total + 1 >= capacity || total >= HEADER_LIMIT_BYTES + max_body_bytes) {
            snprintf(error_buf, error_buf_size, "HTTP request is too large");
            free(buffer);
            return 0;
        }

        chunk = (int)(capacity - total - 1);
        if (chunk > 4096) {
            chunk = 4096;
        }

        received = (int)recv(client, buffer + total, chunk, 0);
        if (received <= 0) {
            snprintf(error_buf, error_buf_size, "failed to read HTTP request");
            free(buffer);
            return 0;
        }

        total += (size_t)received;
        buffer[total] = '\0';
        header_end = find_header_end(buffer, total);
    }

    if (sscanf(buffer, "%7s %127s", request->method, request->path) != 2) {
        snprintf(error_buf, error_buf_size, "invalid HTTP request line");
        free(buffer);
        return 0;
    }

    if (!parse_content_length(buffer, header_end, &content_length)) {
        snprintf(error_buf, error_buf_size, "invalid Content-Length header");
        free(buffer);
        return 0;
    }

    if (content_length > max_body_bytes) {
        snprintf(error_buf, error_buf_size, "request body exceeds max-body limit");
        free(buffer);
        return 0;
    }

    while (total < header_end + content_length) {
        int received;
        int chunk = (int)(header_end + content_length - total);

        if (chunk > 4096) {
            chunk = 4096;
        }

        received = (int)recv(client, buffer + total, chunk, 0);
        if (received <= 0) {
            snprintf(error_buf, error_buf_size, "failed to read HTTP request body");
            free(buffer);
            return 0;
        }

        total += (size_t)received;
    }

    request->body = duplicate_range(buffer + header_end, content_length);
    if (request->body == NULL) {
        snprintf(error_buf, error_buf_size, "failed to allocate request body");
        free(buffer);
        return 0;
    }

    request->body_len = content_length;
    free(buffer);
    return 1;
}

/*
 * raw text body 로 들어온 SQL 의 앞뒤 공백을 제거한다.
 * Thunder Client 의 Body > Text 탭에서 SQL 을 붙여 넣는 경우 이 경로를 탄다.
 */
static char *duplicate_trimmed_body(const char *body, size_t body_len) {
    size_t start = 0;
    size_t end = body_len;

    while (start < body_len && isspace((unsigned char)body[start])) {
        ++start;
    }

    while (end > start && isspace((unsigned char)body[end - 1])) {
        --end;
    }

    return duplicate_range(body + start, end - start);
}

/*
 * JSON body 에서 "sql" 필드만 꺼내는 작은 파서다.
 * 과제 API 계약이 단순하므로 외부 JSON 라이브러리 없이 필요한 범위만 처리한다.
 */
static char *extract_json_sql_string(const char *body, size_t body_len) {
    size_t index;

    /*
     * This is intentionally a tiny parser for the demo API contract:
     * {"sql":"SELECT * FROM users;"}. Raw SQL bodies are handled elsewhere.
     */
    for (index = 0; index + 5 < body_len; ++index) {
        if (body[index] == '"' &&
            index + 5 < body_len &&
            body[index + 1] == 's' &&
            body[index + 2] == 'q' &&
            body[index + 3] == 'l' &&
            body[index + 4] == '"') {
            size_t cursor = index + 5;
            size_t out_len = 0;
            char *out;

            while (cursor < body_len && isspace((unsigned char)body[cursor])) {
                ++cursor;
            }
            if (cursor >= body_len || body[cursor] != ':') {
                continue;
            }
            ++cursor;
            while (cursor < body_len && isspace((unsigned char)body[cursor])) {
                ++cursor;
            }
            if (cursor >= body_len || body[cursor] != '"') {
                continue;
            }
            ++cursor;

            out = (char *)malloc(body_len - cursor + 1);
            if (out == NULL) {
                return NULL;
            }

            while (cursor < body_len) {
                char current = body[cursor++];

                if (current == '"') {
                    out[out_len] = '\0';
                    return out;
                }

                if (current == '\\' && cursor < body_len) {
                    char escaped = body[cursor++];
                    if (escaped == 'n') {
                        current = '\n';
                    } else if (escaped == 'r') {
                        current = '\r';
                    } else if (escaped == 't') {
                        current = '\t';
                    } else {
                        current = escaped;
                    }
                }

                out[out_len++] = current;
            }

            free(out);
            return NULL;
        }
    }

    return NULL;
}

/*
 * /query body 를 최종 SQL 문자열로 변환한다.
 *
 * 분기:
 * - body 첫 글자가 '{' 이면 {"sql":"..."} 형태로 보고 extract_json_sql_string()
 * - 아니면 raw SQL 로 보고 duplicate_trimmed_body()
 */
static char *extract_sql_text(const char *body, size_t body_len) {
    size_t index = 0;

    while (index < body_len && isspace((unsigned char)body[index])) {
        ++index;
    }

    if (index < body_len && body[index] == '{') {
        return extract_json_sql_string(body, body_len);
    }

    return duplicate_trimmed_body(body, body_len);
}

/*
 * 엔진 출력/에러 문자열을 JSON 문자열 값에 안전하게 넣기 위해 escape 한다.
 * 예: 줄바꿈은 \n, 큰따옴표는 \" 로 바꾼다.
 */
static char *json_escape(const char *text) {
    size_t input_len = strlen(text);
    size_t capacity = input_len * 6 + 1;
    char *escaped = (char *)malloc(capacity);
    size_t in_index;
    size_t out_index = 0;

    if (escaped == NULL) {
        return NULL;
    }

    for (in_index = 0; in_index < input_len; ++in_index) {
        unsigned char current = (unsigned char)text[in_index];

        if (current == '"' || current == '\\') {
            escaped[out_index++] = '\\';
            escaped[out_index++] = (char)current;
        } else if (current == '\n') {
            escaped[out_index++] = '\\';
            escaped[out_index++] = 'n';
        } else if (current == '\r') {
            escaped[out_index++] = '\\';
            escaped[out_index++] = 'r';
        } else if (current == '\t') {
            escaped[out_index++] = '\\';
            escaped[out_index++] = 't';
        } else if (current < 0x20) {
            snprintf(escaped + out_index, capacity - out_index, "\\u%04x", current);
            out_index += 6;
        } else {
            escaped[out_index++] = (char)current;
        }
    }

    escaped[out_index] = '\0';
    return escaped;
}

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} StringBuilder;

typedef struct {
    const char *start;
    size_t length;
} CsvFieldRef;

/*
 * 응답 JSON 은 길이가 동적으로 커진다.
 * StringBuilder 는 realloc 으로 버퍼를 키우며 문자열을 안전하게 이어 붙이는 도구다.
 */
static int builder_init(StringBuilder *builder, size_t initial_capacity) {
    builder->data = (char *)malloc(initial_capacity);
    if (builder->data == NULL) {
        return 0;
    }

    builder->length = 0;
    builder->capacity = initial_capacity;
    builder->data[0] = '\0';
    return 1;
}

static void builder_free(StringBuilder *builder) {
    free(builder->data);
    builder->data = NULL;
    builder->length = 0;
    builder->capacity = 0;
}

/*
 * 현재 capacity 로 부족하면 2배씩 늘린다.
 * append 마다 정확한 크기로 realloc 하지 않기 때문에 큰 SELECT 결과도 비교적 효율적으로 만든다.
 */
static int builder_reserve(StringBuilder *builder, size_t additional) {
    size_t required = builder->length + additional + 1;
    size_t new_capacity;
    char *new_data;

    if (required <= builder->capacity) {
        return 1;
    }

    new_capacity = builder->capacity == 0 ? 256 : builder->capacity;
    while (new_capacity < required) {
        new_capacity *= 2;
    }

    new_data = (char *)realloc(builder->data, new_capacity);
    if (new_data == NULL) {
        return 0;
    }

    builder->data = new_data;
    builder->capacity = new_capacity;
    return 1;
}

static int builder_append(StringBuilder *builder, const char *text) {
    size_t length = strlen(text);

    if (!builder_reserve(builder, length)) {
        return 0;
    }

    memcpy(builder->data + builder->length, text, length + 1);
    builder->length += length;
    return 1;
}

static int builder_append_char(StringBuilder *builder, char value) {
    if (!builder_reserve(builder, 1)) {
        return 0;
    }

    builder->data[builder->length++] = value;
    builder->data[builder->length] = '\0';
    return 1;
}

static int builder_append_json_string_range(StringBuilder *builder, const char *start, size_t length) {
    char *temporary = duplicate_range(start, length);
    char *escaped;
    int ok;

    if (temporary == NULL) {
        return 0;
    }

    escaped = json_escape(temporary);
    free(temporary);
    if (escaped == NULL) {
        return 0;
    }

    ok = builder_append_char(builder, '"') &&
         builder_append(builder, escaped) &&
         builder_append_char(builder, '"');
    free(escaped);
    return ok;
}

static size_t trim_line_length(const char *start, size_t length) {
    while (length > 0 && (start[length - 1] == '\r' || start[length - 1] == '\n')) {
        --length;
    }

    return length;
}

static int is_identifier_csv_header(const char *start, size_t length) {
    size_t index;
    int saw_name_char = 0;

    if (length == 0) {
        return 0;
    }

    for (index = 0; index < length; ++index) {
        unsigned char current = (unsigned char)start[index];
        if (isalnum(current) || current == '_') {
            saw_name_char = 1;
            continue;
        }
        if (current == ',') {
            continue;
        }
        return 0;
    }

    return saw_name_char;
}

static size_t count_csv_fields(const char *start, size_t length) {
    size_t count = 1;
    size_t index;

    if (length == 0) {
        return 0;
    }

    for (index = 0; index < length; ++index) {
        if (start[index] == ',') {
            ++count;
        }
    }

    return count;
}

static void fill_csv_field_refs(const char *start, size_t length, CsvFieldRef *fields, size_t field_count) {
    size_t index;
    size_t field_index = 0;
    const char *field_start = start;

    for (index = 0; index <= length && field_index < field_count; ++index) {
        if (index == length || start[index] == ',') {
            fields[field_index].start = field_start;
            fields[field_index].length = (size_t)((start + index) - field_start);
            field_start = start + index + 1;
            ++field_index;
        }
    }

    while (field_index < field_count) {
        fields[field_index].start = "";
        fields[field_index].length = 0;
        ++field_index;
    }
}

/*
 * 기존 SQL 엔진은 결과를 CSV 텍스트로 출력한다.
 * 이 함수는 그 CSV 를 분석해서 API 응답에 columns/rows/rowCount 를 추가한다.
 *
 * 예:
 *   id,name,email,age\n
 *   2,KIM,a@b.com,20\n
 *
 * 변환:
 *   "columns":["id","name","email","age"],
 *   "rows":[{"id":"2","name":"KIM","email":"a@b.com","age":"20"}],
 *   "rowCount":1
 *
 * 왜 필요한가:
 * Thunder Client 에서 단순 문자열보다 JSON row 배열이 훨씬 확인하기 쉽다.
 */
static int append_table_projection_json(StringBuilder *builder, const char *csv_output) {
    const char *cursor = csv_output;
    const char *header_start = cursor;
    const char *line_end;
    size_t header_len;
    size_t column_count;
    CsvFieldRef *columns;
    size_t index;
    size_t row_count = 0;

    line_end = strchr(cursor, '\n');
    if (line_end == NULL) {
        line_end = cursor + strlen(cursor);
    }
    header_len = trim_line_length(header_start, (size_t)(line_end - header_start));

    if (!is_identifier_csv_header(header_start, header_len)) {
        return builder_append(builder, "\"columns\":[],\"rows\":[],\"rowCount\":0");
    }

    column_count = count_csv_fields(header_start, header_len);
    columns = (CsvFieldRef *)calloc(column_count, sizeof(CsvFieldRef));
    if (columns == NULL) {
        return 0;
    }
    fill_csv_field_refs(header_start, header_len, columns, column_count);

    /*
     * The engine still returns CSV text for backward compatibility. The API
     * additionally projects that CSV into JSON rows for Thunder Client demos.
     */
    if (!builder_append(builder, "\"columns\":[")) {
        free(columns);
        return 0;
    }
    for (index = 0; index < column_count; ++index) {
        if (index > 0 && !builder_append_char(builder, ',')) {
            free(columns);
            return 0;
        }
        if (!builder_append_json_string_range(builder, columns[index].start, columns[index].length)) {
            free(columns);
            return 0;
        }
    }
    if (!builder_append(builder, "],\"rows\":[")) {
        free(columns);
        return 0;
    }

    cursor = (*line_end == '\n') ? line_end + 1 : line_end;
    while (*cursor != '\0') {
        CsvFieldRef *values;
        size_t row_len;

        line_end = strchr(cursor, '\n');
        if (line_end == NULL) {
            line_end = cursor + strlen(cursor);
        }
        row_len = trim_line_length(cursor, (size_t)(line_end - cursor));
        if (row_len == 0) {
            cursor = (*line_end == '\n') ? line_end + 1 : line_end;
            continue;
        }

        values = (CsvFieldRef *)calloc(column_count, sizeof(CsvFieldRef));
        if (values == NULL) {
            free(columns);
            return 0;
        }
        fill_csv_field_refs(cursor, row_len, values, column_count);

        if (row_count > 0 && !builder_append_char(builder, ',')) {
            free(values);
            free(columns);
            return 0;
        }
        if (!builder_append_char(builder, '{')) {
            free(values);
            free(columns);
            return 0;
        }
        for (index = 0; index < column_count; ++index) {
            if (index > 0 && !builder_append_char(builder, ',')) {
                free(values);
                free(columns);
                return 0;
            }
            if (!builder_append_json_string_range(builder, columns[index].start, columns[index].length) ||
                !builder_append_char(builder, ':') ||
                !builder_append_json_string_range(builder, values[index].start, values[index].length)) {
                free(values);
                free(columns);
                return 0;
            }
        }
        if (!builder_append_char(builder, '}')) {
            free(values);
            free(columns);
            return 0;
        }

        ++row_count;
        free(values);
        cursor = (*line_end == '\n') ? line_end + 1 : line_end;
    }

    {
        char suffix[64];
        snprintf(suffix, sizeof(suffix), "],\"rowCount\":%zu", row_count);
        if (!builder_append(builder, suffix)) {
            free(columns);
            return 0;
        }
    }

    free(columns);
    return 1;
}

/*
 * append_table_projection_json() 의 결과를 heap 문자열로 반환하는 wrapper.
 * 실패하면 NULL 이고, 성공하면 caller 가 free 해야 한다.
 */
static char *format_table_projection_json(const char *csv_output) {
    StringBuilder builder;

    if (!builder_init(&builder, 512)) {
        return NULL;
    }

    if (!append_table_projection_json(&builder, csv_output == NULL ? "" : csv_output)) {
        builder_free(&builder);
        return NULL;
    }

    return builder.data;
}

/*
 * DbApiResult 를 최종 HTTP response body 로 바꾼다.
 *
 * 성공 분기:
 * - result: 기존 엔진의 CSV 원문
 * - columns/rows/rowCount: Thunder Client 용 구조화 결과
 * - stats: 인덱스 사용 여부, 스캔/매칭 row 수, 실행 시간
 *
 * 실패 분기:
 * - error 문자열만 담아 400 응답으로 내려간다.
 */
static char *format_query_body(const DbApiResult *result) {
    char *escaped;
    char *table_projection;
    char *body;
    size_t needed;

    if (result->ok) {
        escaped = json_escape(result->output == NULL ? "" : result->output);
        if (escaped == NULL) {
            return NULL;
        }

        table_projection = format_table_projection_json(result->output == NULL ? "" : result->output);
        if (table_projection == NULL) {
            free(escaped);
            return NULL;
        }

        needed = strlen(escaped) + strlen(table_projection) + 512;
        body = (char *)malloc(needed);
        if (body == NULL) {
            free(table_projection);
            free(escaped);
            return NULL;
        }

        snprintf(body,
                 needed,
                 "{\"ok\":true,\"result\":\"%s\",%s,\"stats\":{\"usedIndex\":%s,\"scannedRows\":%zu,\"matchedRows\":%zu,\"elapsedMs\":%.3f}}",
                 escaped,
                 table_projection,
                 result->stats.used_index ? "true" : "false",
                 result->stats.scanned_rows,
                 result->stats.matched_rows,
                 result->stats.elapsed_ms);
        free(table_projection);
        free(escaped);
        return body;
    }

    escaped = json_escape(result->error);
    if (escaped == NULL) {
        return NULL;
    }

    needed = strlen(escaped) + 64;
    body = (char *)malloc(needed);
    if (body == NULL) {
        free(escaped);
        return NULL;
    }

    snprintf(body, needed, "{\"ok\":false,\"error\":\"%s\"}", escaped);
    free(escaped);
    return body;
}

/*
 * 여러 worker thread 가 동시에 요청 수를 올릴 수 있으므로 metrics_mutex 로 보호한다.
 */
static void metrics_record(ApiServer *server, int is_query, int ok) {
    metrics_mutex_lock(&server->metrics_mutex);
    ++server->metrics.total_requests;
    if (is_query) {
        ++server->metrics.query_requests;
    }
    if (ok) {
        ++server->metrics.ok_responses;
    } else {
        ++server->metrics.error_responses;
    }
    metrics_mutex_unlock(&server->metrics_mutex);
}

static ApiMetrics metrics_snapshot(ApiServer *server) {
    ApiMetrics snapshot;

    metrics_mutex_lock(&server->metrics_mutex);
    snapshot = server->metrics;
    metrics_mutex_unlock(&server->metrics_mutex);

    return snapshot;
}

static char *format_metrics_body(ApiServer *server) {
    ApiMetrics snapshot = metrics_snapshot(server);
    char *body = (char *)malloc(256);

    if (body == NULL) {
        return NULL;
    }

    snprintf(body,
             256,
             "{\"ok\":true,\"metrics\":{\"totalRequests\":%zu,\"queryRequests\":%zu,\"okResponses\":%zu,\"errorResponses\":%zu}}",
             snapshot.total_requests,
             snapshot.query_requests,
             snapshot.ok_responses,
             snapshot.error_responses);

    return body;
}

/*
 * send() 는 요청한 byte 를 한 번에 모두 보내지 못할 수 있다.
 * 그래서 남은 길이가 0 이 될 때까지 반복 전송한다.
 */
static int send_all(ApiSocket client, const char *data, size_t length) {
    size_t sent = 0;

    while (sent < length) {
        int chunk = (int)(length - sent);
        int sent_now;

        if (chunk > 16384) {
            chunk = 16384;
        }

        sent_now = (int)send(client, data + sent, chunk, 0);
        if (sent_now <= 0) {
            return 0;
        }

        sent += (size_t)sent_now;
    }

    return 1;
}

/*
 * HTTP header 와 JSON body 를 클라이언트에게 보낸다.
 * 이 서버는 요청 1개 처리 후 연결을 닫으므로 Connection: close 를 명시한다.
 */
static int send_json(ApiSocket client, int status, const char *reason, const char *body) {
    char header[512];
    size_t body_len = strlen(body);
    int header_len;

    header_len = snprintf(header,
                          sizeof(header),
                          "HTTP/1.1 %d %s\r\n"
                          "Content-Type: application/json; charset=utf-8\r\n"
                          "Content-Length: %zu\r\n"
                          "Connection: close\r\n"
                          "Access-Control-Allow-Origin: *\r\n"
                          "Access-Control-Allow-Headers: Content-Type\r\n"
                          "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                          "\r\n",
                          status,
                          reason,
                          body_len);

    if (header_len <= 0 || (size_t)header_len >= sizeof(header)) {
        return 0;
    }

    return send_all(client, header, (size_t)header_len) && send_all(client, body, body_len);
}

/*
 * 라우팅/파싱 단계에서 실패했을 때 공통 JSON 에러를 내려준다.
 */
static void send_error(ApiServer *server, ApiSocket client, int status, const char *reason, const char *message) {
    char *escaped = json_escape(message);
    char *body;
    size_t needed;

    if (escaped == NULL) {
        send_json(client, 500, "Internal Server Error", "{\"ok\":false,\"error\":\"out of memory\"}");
        metrics_record(server, 0, 0);
        return;
    }

    needed = strlen(escaped) + 64;
    body = (char *)malloc(needed);
    if (body == NULL) {
        free(escaped);
        send_json(client, 500, "Internal Server Error", "{\"ok\":false,\"error\":\"out of memory\"}");
        metrics_record(server, 0, 0);
        return;
    }

    snprintf(body, needed, "{\"ok\":false,\"error\":\"%s\"}", escaped);
    send_json(client, status, reason, body);
    metrics_record(server, 0, 0);
    free(body);
    free(escaped);
}

/*
 * POST /query 의 핵심 처리 함수.
 *
 * 흐름:
 * 1. HTTP body 에서 SQL 문자열 추출
 * 2. DbApi 를 통해 기존 SQL 엔진 실행
 * 3. 엔진 결과를 JSON body 로 변환
 * 4. 성공이면 200, SQL/파싱 실패면 400 응답
 */
static void handle_query(ApiServer *server, ApiSocket client, const HttpRequest *request) {
    char *sql_text;
    DbApiResult result;
    char *response_body;
    int ok;

    /*
     * Thunder Client can send raw text/plain SQL, while automated clients can
     * send JSON. Both forms end up as a plain SQL string before hitting DbApi.
     */
    sql_text = extract_sql_text(request->body, request->body_len);
    if (sql_text == NULL || sql_text[0] == '\0') {
        free(sql_text);
        send_json(client, 400, "Bad Request", "{\"ok\":false,\"error\":\"missing SQL text\"}");
        metrics_record(server, 1, 0);
        return;
    }

    memset(&result, 0, sizeof(result));
    ok = db_api_execute_sql(&server->db, sql_text, &result);
    response_body = format_query_body(&result);
    if (response_body == NULL) {
        db_api_result_free(&result);
        free(sql_text);
        send_json(client, 500, "Internal Server Error", "{\"ok\":false,\"error\":\"failed to format response\"}");
        metrics_record(server, 1, 0);
        return;
    }

    if (ok) {
        send_json(client, 200, "OK", response_body);
        metrics_record(server, 1, 1);
    } else {
        send_json(client, 400, "Bad Request", response_body);
        metrics_record(server, 1, 0);
    }

    free(response_body);
    db_api_result_free(&result);
    free(sql_text);
}

/*
 * worker thread 가 실제로 실행하는 함수.
 *
 * 분기 시나리오:
 * - HTTP 요청 자체를 못 읽음     -> 400 Bad Request
 * - OPTIONS                     -> CORS 사전 요청 성공
 * - GET /health                 -> 서버 alive 확인
 * - GET /metrics                -> 누적 통계 확인
 * - POST /query                 -> SQL 처리(handle_query)
 * - 등록되지 않은 method/path    -> 404 Not Found
 *
 * task 는 api_server_run() 의 accept loop 에서 malloc 되었으므로
 * 함수 시작 직후 free 한다. client socket 은 응답 후 항상 닫는다.
 */
static void handle_client(void *arg) {
    ClientTask *task = (ClientTask *)arg;
    ApiServer *server = task->server;
    ApiSocket client = task->client;
    HttpRequest request;
    char error[256];

    free(task);
    memset(error, 0, sizeof(error));

    if (!read_http_request(client, server->max_body_bytes, &request, error, sizeof(error))) {
        send_error(server, client, 400, "Bad Request", error);
        API_CLOSE_SOCKET(client);
        return;
    }

    if (strcmp(request.method, "OPTIONS") == 0) {
        send_json(client, 200, "OK", "{\"ok\":true}");
        metrics_record(server, 0, 1);
    } else if (strcmp(request.method, "GET") == 0 && strcmp(request.path, "/health") == 0) {
        send_json(client, 200, "OK", "{\"ok\":true,\"status\":\"ready\"}");
        metrics_record(server, 0, 1);
    } else if (strcmp(request.method, "GET") == 0 && strcmp(request.path, "/metrics") == 0) {
        char *body = format_metrics_body(server);
        if (body == NULL) {
            send_json(client, 500, "Internal Server Error", "{\"ok\":false,\"error\":\"out of memory\"}");
            metrics_record(server, 0, 0);
        } else {
            send_json(client, 200, "OK", body);
            metrics_record(server, 0, 1);
            free(body);
        }
    } else if (strcmp(request.method, "POST") == 0 && strcmp(request.path, "/query") == 0) {
        handle_query(server, client, &request);
    } else {
        send_json(client, 404, "Not Found", "{\"ok\":false,\"error\":\"route not found\"}");
        metrics_record(server, 0, 0);
    }

    request_free(&request);
    API_CLOSE_SOCKET(client);
}

/*
 * Windows 에서는 소켓 API 사용 전에 WSAStartup 이 필요하다.
 * Linux/macOS 에서는 별도 초기화가 필요 없어서 no-op 이다.
 */
static int network_startup(char *error_buf, size_t error_buf_size) {
#if defined(_WIN32)
    WSADATA data;
    int rc = WSAStartup(MAKEWORD(2, 2), &data);

    if (rc != 0) {
        snprintf(error_buf, error_buf_size, "WSAStartup failed: %d", rc);
        return 0;
    }
#else
    (void)error_buf;
    (void)error_buf_size;
#endif
    return 1;
}

static void network_cleanup(void) {
#if defined(_WIN32)
    WSACleanup();
#endif
}

/*
 * host:port 로 listen socket 을 만든다.
 *
 * 단계:
 * 1. getaddrinfo 로 bind 가능한 주소 후보를 얻음
 * 2. socket 생성
 * 3. bind 로 host:port 점유
 * 4. listen 으로 accept 가능한 상태 전환
 */
static ApiSocket open_listener(const char *host, int port, char *error_buf, size_t error_buf_size) {
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    struct addrinfo *item;
    char port_text[16];
    ApiSocket listener = API_INVALID_SOCKET;
    int rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    snprintf(port_text, sizeof(port_text), "%d", port);
    rc = getaddrinfo(host, port_text, &hints, &result);
    if (rc != 0) {
#if defined(_WIN32)
        snprintf(error_buf, error_buf_size, "getaddrinfo failed: %d", rc);
#else
        snprintf(error_buf, error_buf_size, "getaddrinfo failed: %s", gai_strerror(rc));
#endif
        return API_INVALID_SOCKET;
    }

    for (item = result; item != NULL; item = item->ai_next) {
        int yes = 1;

        listener = (ApiSocket)socket(item->ai_family, item->ai_socktype, item->ai_protocol);
        if (listener == API_INVALID_SOCKET) {
            continue;
        }

        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));

        if (bind(listener, item->ai_addr, (int)item->ai_addrlen) == 0) {
            break;
        }

        API_CLOSE_SOCKET(listener);
        listener = API_INVALID_SOCKET;
    }

    freeaddrinfo(result);

    if (listener == API_INVALID_SOCKET) {
        snprintf(error_buf,
                 error_buf_size,
                 "failed to bind %s:%d, socket_error=%d",
                 host == NULL ? "0.0.0.0" : host,
                 port,
                 API_SOCKET_ERROR_TEXT());
        return API_INVALID_SOCKET;
    }

    if (listen(listener, SOMAXCONN) != 0) {
        snprintf(error_buf, error_buf_size, "listen failed, socket_error=%d", API_SOCKET_ERROR_TEXT());
        API_CLOSE_SOCKET(listener);
        return API_INVALID_SOCKET;
    }

    return listener;
}

/*
 * API 서버의 최상위 실행 함수.
 *
 * 시퀀스:
 * 1. metrics mutex 준비
 * 2. DbApi 초기화: data_dir 의 테이블/인덱스를 사용할 준비
 * 3. thread pool 생성: 요청 병렬 처리를 위한 worker 준비
 * 4. network_startup/open_listener
 * 5. accept 무한 루프
 * 6. 연결 1개마다 ClientTask 를 만들어 thread_pool_submit()
 *
 * 중요한 분리:
 * - accept loop 는 "연결을 받는 일"만 한다.
 * - handle_client 는 worker thread 안에서 "요청을 처리하는 일"을 한다.
 */
int api_server_run(const ApiServerConfig *config, char *error_buf, size_t error_buf_size) {
    ApiServer server;
    ApiSocket listener;
    const char *host;

    if (config == NULL) {
        snprintf(error_buf, error_buf_size, "server config is required");
        return 0;
    }

    host = (config->host != NULL && config->host[0] != '\0') ? config->host : "0.0.0.0";
    memset(&server, 0, sizeof(server));
    server.max_body_bytes = config->max_body_bytes == 0 ? 65536 : config->max_body_bytes;

    if (!metrics_mutex_init(&server.metrics_mutex)) {
        snprintf(error_buf, error_buf_size, "failed to initialize metrics mutex");
        return 0;
    }

    /*
     * DbApi owns the existing SQL engine. It serializes engine access because
     * the week7 storage and B+ tree code were not written as thread-safe data
     * structures. The API server can still accept many clients concurrently.
     */
    if (!db_api_init(&server.db, config->data_dir, error_buf, error_buf_size)) {
        metrics_mutex_destroy(&server.metrics_mutex);
        return 0;
    }

    server.pool = thread_pool_create(config->thread_count,
                                     config->queue_capacity,
                                     error_buf,
                                     error_buf_size);
    if (server.pool == NULL) {
        db_api_free(&server.db);
        metrics_mutex_destroy(&server.metrics_mutex);
        return 0;
    }

    if (!network_startup(error_buf, error_buf_size)) {
        thread_pool_destroy(server.pool);
        db_api_free(&server.db);
        metrics_mutex_destroy(&server.metrics_mutex);
        return 0;
    }

    listener = open_listener(host, config->port, error_buf, error_buf_size);
    if (listener == API_INVALID_SOCKET) {
        network_cleanup();
        thread_pool_destroy(server.pool);
        db_api_free(&server.db);
        metrics_mutex_destroy(&server.metrics_mutex);
        return 0;
    }

    printf("Mini SQL API server listening on http://%s:%d\n", host, config->port);
    printf("Try: GET /health, POST /query, GET /metrics\n");
    fflush(stdout);

    while (1) {
        ApiSocket client;
        ClientTask *task;

        client = (ApiSocket)accept(listener, NULL, NULL);
        if (client == API_INVALID_SOCKET) {
            snprintf(error_buf, error_buf_size, "accept failed, socket_error=%d", API_SOCKET_ERROR_TEXT());
            break;
        }

        task = (ClientTask *)malloc(sizeof(*task));
        if (task == NULL) {
            API_CLOSE_SOCKET(client);
            continue;
        }

        task->server = &server;
        task->client = client;

        /*
         * Accept thread only places work into the queue. Worker threads do the
         * HTTP parsing and SQL execution, which is the required thread-pool
         * architecture for this assignment.
         */
        if (!thread_pool_submit(server.pool, handle_client, task)) {
            API_CLOSE_SOCKET(client);
            free(task);
        }
    }

    API_CLOSE_SOCKET(listener);
    network_cleanup();
    thread_pool_destroy(server.pool);
    db_api_free(&server.db);
    metrics_mutex_destroy(&server.metrics_mutex);
    return 0;
}
