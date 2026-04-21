#include "db_api.h"
#include "thread_pool.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <windows.h>
typedef CRITICAL_SECTION TestMutex;

static void test_mutex_init(TestMutex *mutex) {
    InitializeCriticalSection(mutex);
}

static void test_mutex_destroy(TestMutex *mutex) {
    DeleteCriticalSection(mutex);
}

static void test_mutex_lock(TestMutex *mutex) {
    EnterCriticalSection(mutex);
}

static void test_mutex_unlock(TestMutex *mutex) {
    LeaveCriticalSection(mutex);
}
#else
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
typedef pthread_mutex_t TestMutex;

static void test_mutex_init(TestMutex *mutex) {
    pthread_mutex_init(mutex, NULL);
}

static void test_mutex_destroy(TestMutex *mutex) {
    pthread_mutex_destroy(mutex);
}

static void test_mutex_lock(TestMutex *mutex) {
    pthread_mutex_lock(mutex);
}

static void test_mutex_unlock(TestMutex *mutex) {
    pthread_mutex_unlock(mutex);
}
#endif

typedef struct {
    int counter;
    TestMutex mutex;
} CounterContext;

static void fail(const char *message) {
    fprintf(stderr, "[FAIL] %s\n", message);
    exit(1);
}

static void expect(int condition, const char *message) {
    if (!condition) {
        fail(message);
    }
}

static void ensure_directory(const char *path) {
    int result;

#if defined(_WIN32)
    result = _mkdir(path);
#else
    result = mkdir(path, 0777);
#endif

    if (result == 0 || errno == EEXIST) {
        return;
    }

    fail("failed to create test directory");
}

static void write_file(const char *path, const char *contents) {
    FILE *file = fopen(path, "wb");

    if (file == NULL) {
        fail("failed to create test file");
    }

    fputs(contents, file);
    fclose(file);
}

static void increment_task(void *arg) {
    CounterContext *context = (CounterContext *)arg;

    /*
     * This task is intentionally small. The test proves that queued work runs
     * on worker threads and that thread_pool_destroy waits for queued tasks.
     */
    test_mutex_lock(&context->mutex);
    ++context->counter;
    test_mutex_unlock(&context->mutex);
}

static void test_thread_pool_executes_queued_tasks(void) {
    ThreadPool *pool;
    CounterContext context;
    char error[256];
    int index;

    memset(&context, 0, sizeof(context));
    memset(error, 0, sizeof(error));
    test_mutex_init(&context.mutex);

    pool = thread_pool_create(4, 16, error, sizeof(error));
    expect(pool != NULL, error);

    for (index = 0; index < 64; ++index) {
        expect(thread_pool_submit(pool, increment_task, &context), "thread pool submit failed");
    }

    thread_pool_destroy(pool);
    expect(context.counter == 64, "thread pool did not execute every queued task");
    test_mutex_destroy(&context.mutex);
}

static void test_db_api_captures_engine_output(void) {
    DbApi api;
    DbApiResult result;
    char error[512];

    /*
     * DbApi is the boundary between HTTP worker threads and the old week7 SQL
     * engine. It captures FILE* output into a heap string so the server can
     * place query results inside a JSON response.
     */
    ensure_directory("test_api_data");
    remove("test_api_data/users.tbl");
    remove("test_api_data/users.idx");
    write_file("test_api_data/users.tbl",
               "id,name,email,age\n"
               "1,Alice,alice@example.com,20\n"
               "2,Bob,bob@example.com,25\n");

    memset(error, 0, sizeof(error));
    expect(db_api_init(&api, "test_api_data", error, sizeof(error)), error);

    memset(&result, 0, sizeof(result));
    expect(db_api_execute_sql(&api, "SELECT name FROM users WHERE id = 2;", &result), result.error);
    expect(strstr(result.output, "name\nBob\n") != NULL, "SELECT result was not captured");
    expect(result.stats.used_index == 1, "id lookup should use B+ tree index");
    db_api_result_free(&result);

    memset(&result, 0, sizeof(result));
    expect(db_api_execute_sql(&api,
                              "INSERT INTO users (email, name, age) VALUES ('carol@example.com', 'Carol', 30);",
                              &result),
           result.error);
    expect(strstr(result.output, "Inserted 1 row") != NULL, "INSERT response was not captured");
    db_api_result_free(&result);

    db_api_free(&api);
}

static void test_db_api_rebuilds_stale_index(void) {
    DbApi api;
    DbApiResult result;
    char error[512];

    /*
     * Demo data is often copied between folders. This test simulates users.tbl
     * changing after users.idx was already built, then checks that startup
     * rebuilds the index instead of returning an empty header-only result.
     */
    ensure_directory("test_stale_index_data");
    remove("test_stale_index_data/users.tbl");
    remove("test_stale_index_data/users.idx");
    write_file("test_stale_index_data/users.tbl",
               "id,name,email,age\n"
               "1,Alice,alice@example.com,20\n");

    memset(error, 0, sizeof(error));
    expect(db_api_init(&api, "test_stale_index_data", error, sizeof(error)), error);
    memset(&result, 0, sizeof(result));
    expect(db_api_execute_sql(&api, "SELECT * FROM users WHERE id = 1;", &result), result.error);
    db_api_result_free(&result);
    db_api_free(&api);

    write_file("test_stale_index_data/users.tbl",
               "id,name,email,age\n"
               "1,Alice,alice@example.com,20\n"
               "2,Bob,bob@example.com,25\n");

    memset(error, 0, sizeof(error));
    expect(db_api_init(&api, "test_stale_index_data", error, sizeof(error)), error);
    memset(&result, 0, sizeof(result));
    expect(db_api_execute_sql(&api, "SELECT * FROM users WHERE id = 2;", &result), result.error);
    expect(strstr(result.output, "2,Bob,bob@example.com,25") != NULL, "stale index was not rebuilt");
    expect(result.stats.matched_rows == 1, "rebuilt index should match id=2");
    db_api_result_free(&result);
    db_api_free(&api);
}

int main(void) {
    test_thread_pool_executes_queued_tasks();
    test_db_api_captures_engine_output();
    test_db_api_rebuilds_stale_index();
    printf("[PASS] api tests completed\n");
    return 0;
}
