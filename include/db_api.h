#ifndef DB_API_H
#define DB_API_H

#include "engine.h"

#include <stddef.h>

typedef struct {
    /* Existing week7 SQL engine instance. */
    SqlEngine engine;

    /*
     * Platform-specific mutex pointer.
     * The actual type is CRITICAL_SECTION on Windows and pthread_mutex_t on POSIX.
     */
    void *mutex_impl;
} DbApi;

typedef struct {
    /* 1 when SQL execution succeeded, 0 when parser/executor/storage failed. */
    int ok;

    /* Captured CSV/text output from the old FILE* based engine API. */
    char *output;

    /* Query path data used by demos: index usage, scanned rows, elapsed time. */
    QueryStats stats;

    /* Human-readable failure message for API JSON responses. */
    char error[512];
} DbApiResult;

int db_api_init(DbApi *api, const char *data_dir, char *error_buf, size_t error_buf_size);
void db_api_free(DbApi *api);
int db_api_execute_sql(DbApi *api, const char *sql_text, DbApiResult *result);
void db_api_result_free(DbApiResult *result);

#endif
