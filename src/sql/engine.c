#include "engine.h"

#include "executor.h"
#include "optimizer.h"
#include "parser.h"
#include "tokenizer.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <direct.h>
#define SQL_MKDIR(path) _mkdir(path)
#else
#include <sys/stat.h>
#include <sys/types.h>
#define SQL_MKDIR(path) mkdir((path), 0777)
#endif

#define QUERY_TIMING_LOG_DIR "logs"
#define QUERY_TIMING_LOG_PATH "logs/query_timing.log"
#define MAX_LOG_SQL_TEXT 200

static char *read_file_to_string(const char *path) {
    FILE *file;
    long length;
    size_t read_size;
    char *buffer;

    file = fopen(path, "rb");
    if (file == NULL) {
        return NULL;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }

    length = ftell(file);
    if (length < 0) {
        fclose(file);
        return NULL;
    }

    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }

    buffer = (char *)malloc((size_t)length + 1);
    if (buffer == NULL) {
        fclose(file);
        return NULL;
    }

    read_size = fread(buffer, 1, (size_t)length, file);
    buffer[read_size] = '\0';
    fclose(file);
    return buffer;
}

static double now_ms(void) {
    struct timespec timestamp;

    timespec_get(&timestamp, TIME_UTC);
    return (double)timestamp.tv_sec * 1000.0 + (double)timestamp.tv_nsec / 1000000.0;
}

static const char *execution_path_text(StatementType type, const QueryStats *stats) {
    if ((int)type < 0) {
        return "unknown";
    }
    if (type == AST_INSERT_STATEMENT) {
        return "insert";
    }
    if (stats != NULL && stats->used_index) {
        return "indexed";
    }
    return "full_scan";
}

static void format_sql_for_log(const char *sql_text, char *buffer, size_t buffer_size) {
    size_t read_index;
    size_t write_index = 0;

    if (buffer_size == 0) {
        return;
    }

    for (read_index = 0; sql_text[read_index] != '\0' && write_index + 1 < buffer_size; ++read_index) {
        char current = sql_text[read_index];
        if (current == '\r' || current == '\n' || current == '\t') {
            current = ' ';
        }
        buffer[write_index++] = current;
    }
    buffer[write_index] = '\0';

    while (write_index > 0 && buffer[write_index - 1] == ' ') {
        buffer[--write_index] = '\0';
    }

    if (sql_text[read_index] != '\0' && buffer_size > 4) {
        buffer[buffer_size - 4] = '.';
        buffer[buffer_size - 3] = '.';
        buffer[buffer_size - 2] = '.';
        buffer[buffer_size - 1] = '\0';
    }
}

static int query_timing_log_enabled(void) {
    const char *value = getenv("MINI_SQL_DISABLE_QUERY_LOG");

    if (value == NULL || value[0] == '\0') {
        return 1;
    }

    return !(strcmp(value, "1") == 0 ||
             strcmp(value, "true") == 0 ||
             strcmp(value, "TRUE") == 0 ||
             strcmp(value, "yes") == 0 ||
             strcmp(value, "YES") == 0);
}

static void append_query_timing_log(const char *mode,
                                    const char *sql_text,
                                    StatementType type,
                                    const QueryStats *stats,
                                    int success,
                                    double elapsed_ms) {
    FILE *file;
    time_t now;
    struct tm *local_time;
    char timestamp[32];
    char formatted_sql[MAX_LOG_SQL_TEXT];
    int mkdir_result;

    if (!query_timing_log_enabled()) {
        return;
    }

    mkdir_result = SQL_MKDIR(QUERY_TIMING_LOG_DIR);
    if (mkdir_result != 0 && errno != EEXIST) {
        return;
    }

    file = fopen(QUERY_TIMING_LOG_PATH, "a");
    if (file == NULL) {
        return;
    }

    now = time(NULL);
    local_time = localtime(&now);
    if (local_time == NULL) {
        snprintf(timestamp, sizeof(timestamp), "unknown-time");
    } else {
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", local_time);
    }

    format_sql_for_log(sql_text, formatted_sql, sizeof(formatted_sql));
    fprintf(file,
            "%s | mode=%s | path=%s | status=%s | time_ms=%.3f | scanned_rows=%zu | matched_rows=%zu | sql=%s\n",
            timestamp,
            mode,
            execution_path_text(type, stats),
            success ? "ok" : "error",
            elapsed_ms,
            stats == NULL ? 0 : stats->scanned_rows,
            stats == NULL ? 0 : stats->matched_rows,
            formatted_sql);
    fclose(file);
}

static int execute_sql_with_mode(SqlEngine *engine,
                                 const char *sql_text,
                                 const char *mode,
                                 FILE *output,
                                 QueryStats *stats,
                                 char *error_buf,
                                 size_t error_buf_size) {
    QueryStats local_stats;
    QueryStats *effective_stats = stats;
    TokenArray tokens;
    Statement statement;
    ExecutorContext context;
    double started_at;
    int ok = 0;
    double elapsed_ms = 0.0;

    if (effective_stats == NULL) {
        memset(&local_stats, 0, sizeof(local_stats));
        effective_stats = &local_stats;
    }

    memset(&tokens, 0, sizeof(tokens));
    memset(&statement, 0, sizeof(statement));
    memset(&context, 0, sizeof(context));
    context.database = &engine->database;
    context.output = output;
    context.stats = effective_stats;
    started_at = now_ms();

    if (!tokenize_sql(sql_text, &tokens, error_buf, error_buf_size)) {
        elapsed_ms = now_ms() - started_at;
        effective_stats->elapsed_ms = elapsed_ms;
        append_query_timing_log(mode, sql_text, (StatementType)-1, effective_stats, 0, elapsed_ms);
        goto cleanup;
    }

    if (!parse_statement(&tokens, &statement, error_buf, error_buf_size)) {
        elapsed_ms = now_ms() - started_at;
        effective_stats->elapsed_ms = elapsed_ms;
        append_query_timing_log(mode, sql_text, (StatementType)-1, effective_stats, 0, elapsed_ms);
        goto cleanup;
    }

    if (!optimize_statement(&statement, error_buf, error_buf_size)) {
        elapsed_ms = now_ms() - started_at;
        effective_stats->elapsed_ms = elapsed_ms;
        append_query_timing_log(mode, sql_text, statement.type, effective_stats, 0, elapsed_ms);
        goto cleanup;
    }

    if (!execute_statement(&statement, &context, error_buf, error_buf_size)) {
        elapsed_ms = now_ms() - started_at;
        effective_stats->elapsed_ms = elapsed_ms;
        append_query_timing_log(mode, sql_text, statement.type, effective_stats, 0, elapsed_ms);
        goto cleanup;
    }

    ok = 1;
    elapsed_ms = now_ms() - started_at;
    effective_stats->elapsed_ms = elapsed_ms;
    append_query_timing_log(mode, sql_text, statement.type, effective_stats, 1, elapsed_ms);

cleanup:
    free_statement(&statement);
    free_tokens(&tokens);
    return ok;
}

int sql_engine_init(SqlEngine *engine, const char *data_dir, char *error_buf, size_t error_buf_size) {
    memset(engine, 0, sizeof(*engine));
    return database_init(&engine->database, data_dir, error_buf, error_buf_size);
}

void sql_engine_free(SqlEngine *engine) {
    if (engine == NULL) {
        return;
    }

    database_free(&engine->database);
}

int sql_engine_execute_sql(SqlEngine *engine,
                           const char *sql_text,
                           FILE *output,
                           QueryStats *stats,
                           char *error_buf,
                           size_t error_buf_size) {
    return execute_sql_with_mode(engine, sql_text, "API", output, stats, error_buf, error_buf_size);
}

int sql_engine_execute_file(SqlEngine *engine,
                            const char *sql_path,
                            FILE *output,
                            QueryStats *stats,
                            char *error_buf,
                            size_t error_buf_size) {
    char *sql_text = read_file_to_string(sql_path);
    int ok;

    if (sql_text == NULL) {
        snprintf(error_buf, error_buf_size, "SQL 파일을 읽을 수 없습니다: %s", sql_path);
        return 0;
    }

    ok = execute_sql_with_mode(engine, sql_text, "FILE", output, stats, error_buf, error_buf_size);
    free(sql_text);
    return ok;
}
