#include "ast.h"
#include "executor.h"
#include "optimizer.h"
#include "parser.h"
#include "tokenizer.h"
#include "trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void print_usage(const char *program_name) {
    fprintf(stderr, "사용법:\n");
    fprintf(stderr, "  %s <sql-file>\n", program_name);
    fprintf(stderr, "  %s --data-dir <dir> <sql-file>\n", program_name);
}

static char *read_file_to_string(const char *path) {
    FILE *file;
    long length;
    size_t read_size;
    char *buffer;

    file = fopen(path, "r");
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

static char *read_stream_to_string(FILE *stream) {
    long length;
    size_t read_size;
    char *buffer;

    if (fseek(stream, 0, SEEK_END) != 0) {
        return NULL;
    }

    length = ftell(stream);
    if (length < 0) {
        return NULL;
    }

    if (fseek(stream, 0, SEEK_SET) != 0) {
        return NULL;
    }

    buffer = (char *)malloc((size_t)length + 1);
    if (buffer == NULL) {
        return NULL;
    }

    read_size = fread(buffer, 1, (size_t)length, stream);
    buffer[read_size] = '\0';
    return buffer;
}

static int execute_statement_captured(const Statement *statement,
                                      const char *data_dir,
                                      char **out_text,
                                      char *error_buf,
                                      size_t error_buf_size) {
    FILE *capture = tmpfile();
    int saved_stdout_fd;
    int ok;

    *out_text = NULL;

    if (capture == NULL) {
        snprintf(error_buf, error_buf_size, "실행 결과 캡처용 임시 파일을 열 수 없습니다.");
        return 0;
    }

    saved_stdout_fd = dup(fileno(stdout));
    if (saved_stdout_fd < 0) {
        fclose(capture);
        snprintf(error_buf, error_buf_size, "stdout 백업에 실패했습니다.");
        return 0;
    }

    fflush(stdout);
    if (dup2(fileno(capture), fileno(stdout)) < 0) {
        close(saved_stdout_fd);
        fclose(capture);
        snprintf(error_buf, error_buf_size, "stdout 리다이렉션에 실패했습니다.");
        return 0;
    }

    ok = execute_statement(statement, data_dir, error_buf, error_buf_size);
    fflush(stdout);

    if (dup2(saved_stdout_fd, fileno(stdout)) < 0) {
        close(saved_stdout_fd);
        fclose(capture);
        snprintf(error_buf, error_buf_size, "stdout 복구에 실패했습니다.");
        return 0;
    }

    close(saved_stdout_fd);

    *out_text = read_stream_to_string(capture);
    fclose(capture);

    if (*out_text == NULL) {
        snprintf(error_buf, error_buf_size, "실행 결과 캡처 내용을 읽는 데 실패했습니다.");
        return 0;
    }

    return ok;
}

static void write_trace_json(const char *sql_text,
                             const TokenArray *tokens,
                             int tokenizer_ok,
                             const Statement *parsed_statement,
                             int parser_ok,
                             const Statement *optimized_statement,
                             int optimizer_ok,
                             const char *executor_output,
                             int executor_ok,
                             const char *error_stage,
                             const char *error_message,
                             int overall_ok) {
    printf("{\"ok\":%s,", overall_ok ? "true" : "false");
    printf("\"sql\":");
    trace_write_json_string(stdout, sql_text);
    printf(",\"stages\":{");

    printf("\"tokenizer\":{\"ok\":%s,\"tokens\":", tokenizer_ok ? "true" : "false");
    trace_write_token_array_json(stdout, tokens);
    printf("},");

    printf("\"parser\":{\"ok\":%s,\"statement\":", parser_ok ? "true" : "false");
    if (parser_ok) {
        trace_write_statement_json(stdout, parsed_statement);
    } else {
        printf("null");
    }
    printf("},");

    printf("\"optimizer\":{\"ok\":%s,\"statement\":", optimizer_ok ? "true" : "false");
    if (optimizer_ok) {
        trace_write_statement_json(stdout, optimized_statement);
    } else {
        printf("null");
    }
    printf("},");

    printf("\"executor\":{\"ok\":%s,\"output\":", executor_ok ? "true" : "false");
    trace_write_json_string(stdout, executor_output == NULL ? "" : executor_output);
    printf("}},");

    printf("\"error\":");
    if (overall_ok) {
        printf("null");
    } else {
        printf("{\"stage\":");
        trace_write_json_string(stdout, error_stage);
        printf(",\"message\":");
        trace_write_json_string(stdout, error_message);
        printf("}");
    }

    printf("}\n");
}

int main(int argc, char **argv) {
    const char *data_dir = "data";
    const char *sql_path = NULL;
    const char *error_stage = "input";
    char error_buf[512] = {0};
    char *sql_text = NULL;
    char *executor_output = NULL;
    TokenArray tokens;
    Statement parsed_statement;
    Statement parsed_snapshot;
    Statement optimized_snapshot;
    int tokenizer_ok = 0;
    int parser_ok = 0;
    int optimizer_ok = 0;
    int executor_ok = 0;
    int overall_ok = 0;

    memset(&tokens, 0, sizeof(tokens));
    memset(&parsed_statement, 0, sizeof(parsed_statement));
    memset(&parsed_snapshot, 0, sizeof(parsed_snapshot));
    memset(&optimized_snapshot, 0, sizeof(optimized_snapshot));

    if (argc == 2) {
        sql_path = argv[1];
    } else if (argc == 4 && strcmp(argv[1], "--data-dir") == 0) {
        data_dir = argv[2];
        sql_path = argv[3];
    } else {
        print_usage(argv[0]);
        return 1;
    }

    sql_text = read_file_to_string(sql_path);
    if (sql_text == NULL) {
        snprintf(error_buf, sizeof(error_buf), "SQL 파일을 읽을 수 없습니다: %s", sql_path);
        write_trace_json("", &tokens, tokenizer_ok, &parsed_snapshot, parser_ok, &optimized_snapshot,
                         optimizer_ok, executor_output, executor_ok, error_stage, error_buf, overall_ok);
        return 1;
    }

    error_stage = "tokenizer";
    if (!tokenize_sql(sql_text, &tokens, error_buf, sizeof(error_buf))) {
        write_trace_json(sql_text, &tokens, tokenizer_ok, &parsed_snapshot, parser_ok, &optimized_snapshot,
                         optimizer_ok, executor_output, executor_ok, error_stage, error_buf, overall_ok);
        free(sql_text);
        return 1;
    }
    tokenizer_ok = 1;

    error_stage = "parser";
    if (!parse_statement(&tokens, &parsed_statement, error_buf, sizeof(error_buf))) {
        write_trace_json(sql_text, &tokens, tokenizer_ok, &parsed_snapshot, parser_ok, &optimized_snapshot,
                         optimizer_ok, executor_output, executor_ok, error_stage, error_buf, overall_ok);
        free_statement(&parsed_statement);
        free_tokens(&tokens);
        free(sql_text);
        return 1;
    }

    if (!clone_statement(&parsed_statement, &parsed_snapshot)) {
        snprintf(error_buf, sizeof(error_buf), "Parser 결과 스냅샷 복제에 실패했습니다.");
        write_trace_json(sql_text, &tokens, tokenizer_ok, &parsed_snapshot, parser_ok, &optimized_snapshot,
                         optimizer_ok, executor_output, executor_ok, error_stage, error_buf, overall_ok);
        free_statement(&parsed_statement);
        free_tokens(&tokens);
        free(sql_text);
        return 1;
    }
    parser_ok = 1;

    error_stage = "optimizer";
    if (!optimize_statement(&parsed_statement, error_buf, sizeof(error_buf))) {
        write_trace_json(sql_text, &tokens, tokenizer_ok, &parsed_snapshot, parser_ok, &optimized_snapshot,
                         optimizer_ok, executor_output, executor_ok, error_stage, error_buf, overall_ok);
        free_statement(&optimized_snapshot);
        free_statement(&parsed_snapshot);
        free_statement(&parsed_statement);
        free_tokens(&tokens);
        free(sql_text);
        return 1;
    }

    if (!clone_statement(&parsed_statement, &optimized_snapshot)) {
        snprintf(error_buf, sizeof(error_buf), "Optimizer 결과 스냅샷 복제에 실패했습니다.");
        write_trace_json(sql_text, &tokens, tokenizer_ok, &parsed_snapshot, parser_ok, &optimized_snapshot,
                         optimizer_ok, executor_output, executor_ok, error_stage, error_buf, overall_ok);
        free_statement(&optimized_snapshot);
        free_statement(&parsed_snapshot);
        free_statement(&parsed_statement);
        free_tokens(&tokens);
        free(sql_text);
        return 1;
    }
    optimizer_ok = 1;

    error_stage = "executor";
    executor_ok = execute_statement_captured(&parsed_statement, data_dir, &executor_output, error_buf, sizeof(error_buf));
    if (!executor_ok) {
        write_trace_json(sql_text, &tokens, tokenizer_ok, &parsed_snapshot, parser_ok, &optimized_snapshot,
                         optimizer_ok, executor_output, executor_ok, error_stage, error_buf, overall_ok);
        free(executor_output);
        free_statement(&optimized_snapshot);
        free_statement(&parsed_snapshot);
        free_statement(&parsed_statement);
        free_tokens(&tokens);
        free(sql_text);
        return 1;
    }

    overall_ok = 1;
    write_trace_json(sql_text, &tokens, tokenizer_ok, &parsed_snapshot, parser_ok, &optimized_snapshot,
                     optimizer_ok, executor_output, executor_ok, error_stage, error_buf, overall_ok);

    free(executor_output);
    free_statement(&optimized_snapshot);
    free_statement(&parsed_snapshot);
    free_statement(&parsed_statement);
    free_tokens(&tokens);
    free(sql_text);
    return 0;
}
