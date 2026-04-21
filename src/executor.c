#include "executor.h"

#include <stdio.h>

int execute_statement(const Statement *statement,
                      const ExecutorContext *context,
                      char *error_buf,
                      size_t error_buf_size) {
    if (statement == NULL) {
        snprintf(error_buf, error_buf_size, "실행할 문장이 없습니다.");
        return 0;
    }
    if (context == NULL || context->database == NULL) {
        snprintf(error_buf, error_buf_size, "실행 컨텍스트가 비어 있습니다.");
        return 0;
    }

    if (statement->type == AST_INSERT_STATEMENT) {
        return database_execute_insert(context->database,
                                       &statement->as.insert_stmt,
                                       context->output,
                                       context->stats,
                                       error_buf,
                                       error_buf_size);
    }

    return database_execute_select(context->database,
                                   &statement->as.select_stmt,
                                   context->output,
                                   context->stats,
                                   error_buf,
                                   error_buf_size);
}
