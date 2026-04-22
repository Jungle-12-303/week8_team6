#include "engine.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#define SQL_ISATTY(fd) _isatty(fd)
#define SQL_STDIN_FD 0
#else
#include <unistd.h>
#define SQL_ISATTY(fd) isatty(fd)
#define SQL_STDIN_FD STDIN_FILENO
#endif

static void print_usage(const char *program_name) {
    fprintf(stderr, "사용법:\n");
    fprintf(stderr, "  %s\n", program_name);
    fprintf(stderr, "  %s --repl\n", program_name);
    fprintf(stderr, "  %s <sql-file>\n", program_name);
    fprintf(stderr, "  %s --data-dir <dir> --repl\n", program_name);
    fprintf(stderr, "  %s --data-dir <dir> <sql-file>\n", program_name);
}

static void trim_newline(char *text) {
    size_t length = strlen(text);

    while (length > 0 && (text[length - 1] == '\n' || text[length - 1] == '\r')) {
        text[length - 1] = '\0';
        --length;
    }
}

static const char *skip_spaces(const char *text) {
    while (*text != '\0' && isspace((unsigned char)*text)) {
        ++text;
    }
    return text;
}

static int is_exit_command(const char *text) {
    const char *trimmed = skip_spaces(text);
    char command[32];
    size_t index = 0;

    while (*trimmed != '\0' && !isspace((unsigned char)*trimmed) && *trimmed != ';' && index + 1 < sizeof(command)) {
        command[index++] = (char)tolower((unsigned char)*trimmed);
        ++trimmed;
    }
    command[index] = '\0';

    trimmed = skip_spaces(trimmed);
    if (*trimmed == ';') {
        ++trimmed;
    }
    trimmed = skip_spaces(trimmed);

    return (*trimmed == '\0') && (strcmp(command, "exit") == 0 || strcmp(command, "quit") == 0);
}

static int starts_with_keyword(const char *text, const char *keyword) {
    const char *trimmed = skip_spaces(text);
    size_t index = 0;

    while (keyword[index] != '\0') {
        if (tolower((unsigned char)trimmed[index]) != tolower((unsigned char)keyword[index])) {
            return 0;
        }
        ++index;
    }

    return trimmed[index] == '\0' || isspace((unsigned char)trimmed[index]) || trimmed[index] == '(';
}

static int append_text(char **buffer, size_t *length, size_t *capacity, const char *text) {
    size_t add_length = strlen(text);
    size_t required = *length + add_length + 1;
    char *new_buffer;
    size_t new_capacity = *capacity == 0 ? 256 : *capacity;

    if (required <= *capacity) {
        memcpy(*buffer + *length, text, add_length + 1);
        *length += add_length;
        return 1;
    }

    while (new_capacity < required) {
        new_capacity *= 2;
    }

    new_buffer = (char *)realloc(*buffer, new_capacity);
    if (new_buffer == NULL) {
        return 0;
    }

    *buffer = new_buffer;
    *capacity = new_capacity;
    memcpy(*buffer + *length, text, add_length + 1);
    *length += add_length;
    return 1;
}

static void reset_statement_buffer(char *buffer, size_t *length) {
    if (buffer != NULL) {
        buffer[0] = '\0';
    }
    *length = 0;
}

static void print_repl_stats(const char *sql_text, const QueryStats *stats) {
    if (starts_with_keyword(sql_text, "select")) {
        fprintf(stderr,
                "[%s] time_ms=%.3f scanned_rows=%zu matched_rows=%zu\n",
                stats->used_index ? "indexed" : "full_scan",
                stats->elapsed_ms,
                stats->scanned_rows,
                stats->matched_rows);
    } else if (starts_with_keyword(sql_text, "insert")) {
        fprintf(stderr, "[insert] time_ms=%.3f\n", stats->elapsed_ms);
    }
}

static int run_repl(SqlEngine *engine, char *error_buf, size_t error_buf_size) {
    int interactive = SQL_ISATTY(SQL_STDIN_FD);
    char line[4096];
    char *statement = NULL;
    size_t statement_length = 0;
    size_t statement_capacity = 0;

    if (interactive) {
        fprintf(stderr, "mini_sql REPL 시작. 종료하려면 EXIT; 를 입력하세요.\n");
    }

    while (1) {
        QueryStats stats;

        if (interactive) {
            fputs(statement_length == 0 ? "sql> " : "...> ", stderr);
            fflush(stderr);
        }

        if (fgets(line, sizeof(line), stdin) == NULL) {
            if (interactive) {
                fputc('\n', stderr);
            }
            break;
        }

        trim_newline(line);
        if (statement_length == 0 && line[0] == '\0') {
            continue;
        }
        if (statement_length == 0 && is_exit_command(line)) {
            break;
        }

        if (!append_text(&statement, &statement_length, &statement_capacity, line) ||
            !append_text(&statement, &statement_length, &statement_capacity, "\n")) {
            fprintf(stderr, "실행 오류: REPL 버퍼 메모리 할당에 실패했습니다.\n");
            free(statement);
            return 0;
        }

        if (strchr(line, ';') == NULL) {
            continue;
        }
        if (is_exit_command(statement)) {
            reset_statement_buffer(statement, &statement_length);
            break;
        }

        memset(&stats, 0, sizeof(stats));
        if (!sql_engine_execute_sql(engine, statement, stdout, &stats, error_buf, error_buf_size)) {
            fprintf(stderr, "실행 오류: %s\n", error_buf);
            reset_statement_buffer(statement, &statement_length);
            continue;
        }

        print_repl_stats(statement, &stats);
        reset_statement_buffer(statement, &statement_length);
    }

    if (statement_length > 0) {
        fprintf(stderr, "입력이 세미콜론(;)으로 끝나지 않아 마지막 문장은 실행하지 않았습니다.\n");
    }

    free(statement);
    return 1;
}

int main(int argc, char **argv) {
    const char *data_dir = "data";
    const char *sql_path = NULL;
    int repl_mode = argc == 1;
    char error_buf[512];
    SqlEngine engine;
    int ok;
    int index;

    memset(error_buf, 0, sizeof(error_buf));
    memset(&engine, 0, sizeof(engine));

    for (index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--data-dir") == 0) {
            if (index + 1 >= argc) {
                print_usage(argv[0]);
                return 1;
            }
            data_dir = argv[++index];
            continue;
        }
        if (strcmp(argv[index], "--repl") == 0) {
            repl_mode = 1;
            continue;
        }
        if (sql_path == NULL) {
            sql_path = argv[index];
            continue;
        }

        print_usage(argv[0]);
        return 1;
    }

    if (repl_mode && sql_path != NULL) {
        print_usage(argv[0]);
        return 1;
    }

    if (!repl_mode && sql_path == NULL) {
        print_usage(argv[0]);
        return 1;
    }

    if (!sql_engine_init(&engine, data_dir, error_buf, sizeof(error_buf))) {
        fprintf(stderr, "초기화 오류: %s\n", error_buf);
        return 1;
    }

    if (repl_mode) {
        ok = run_repl(&engine, error_buf, sizeof(error_buf));
    } else {
        ok = sql_engine_execute_file(&engine, sql_path, stdout, NULL, error_buf, sizeof(error_buf));
        if (!ok) {
            fprintf(stderr, "실행 오류: %s\n", error_buf);
        }
    }

    sql_engine_free(&engine);
    return ok ? 0 : 1;
}
