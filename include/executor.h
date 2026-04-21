#ifndef EXECUTOR_H
#define EXECUTOR_H

#include "ast.h"
#include "database.h"

#include <stddef.h>
#include <stdio.h>

typedef struct {
    Database *database;
    FILE *output;
    QueryStats *stats;
} ExecutorContext;

int execute_statement(const Statement *statement,
                      const ExecutorContext *context,
                      char *error_buf,
                      size_t error_buf_size);

#endif
