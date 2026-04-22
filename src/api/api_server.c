#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200112L
#endif

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

static char *duplicate_range(const char *text, size_t length) {
    char *copy = (char *)malloc(length + 1);

    if (copy == NULL) {
        return NULL;
    }

    memcpy(copy, text, length);
    copy[length] = '\0';
    return copy;
}

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

static void request_free(HttpRequest *request) {
    free(request->body);
    request->body = NULL;
    request->body_len = 0;
}

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
