#ifndef ENGINE_H
#define ENGINE_H

#include "database.h"

#include <stdio.h>

typedef struct {
    Database database;
} SqlEngine;

int sql_engine_init(SqlEngine *engine, const char *data_dir, char *error_buf, size_t error_buf_size);
void sql_engine_free(SqlEngine *engine);
int sql_engine_execute_sql(SqlEngine *engine,
                           const char *sql_text,
                           FILE *output,
                           QueryStats *stats,
                           char *error_buf,
                           size_t error_buf_size);
int sql_engine_execute_file(SqlEngine *engine,
                            const char *sql_path,
                            FILE *output,
                            QueryStats *stats,
                            char *error_buf,
                            size_t error_buf_size);

#endif
