#ifndef DATABASE_H
#define DATABASE_H

#include "ast.h"

#include <stddef.h>
#include <stdio.h>

typedef struct DatabaseTable DatabaseTable;

typedef struct {
    int used_index;
    size_t scanned_rows;
    size_t matched_rows;
    double elapsed_ms;
} QueryStats;

typedef struct {
    char data_dir[1024];
    DatabaseTable *tables;
    size_t table_count;
} Database;

int database_init(Database *database, const char *data_dir, char *error_buf, size_t error_buf_size);
void database_free(Database *database);
int database_execute_select(Database *database,
                            const SelectStatement *select_stmt,
                            FILE *output,
                            QueryStats *stats,
                            char *error_buf,
                            size_t error_buf_size);
int database_execute_insert(Database *database,
                            const InsertStatement *insert_stmt,
                            FILE *output,
                            QueryStats *stats,
                            char *error_buf,
                            size_t error_buf_size);

#endif
