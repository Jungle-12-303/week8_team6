#include "bptree.h"
#include "engine.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

typedef struct {
    int keys[32];
    size_t count;
} KeyCollector;

static void ensure_directory(const char *path);
static void remove_file_if_exists(const char *path);

static void fail(const char *message) {
    fprintf(stderr, "[FAIL] %s\n", message);
    exit(1);
}

static void expect(int condition, const char *message) {
    if (!condition) {
        fail(message);
    }
}

static void remove_file_if_exists(const char *path) {
    remove(path);
}

static int collect_keys(int key, long value, void *context) {
    KeyCollector *collector = (KeyCollector *)context;

    (void)value;

    if (collector->count >= sizeof(collector->keys) / sizeof(collector->keys[0])) {
        return 0;
    }

    collector->keys[collector->count++] = key;
    return 1;
}

static void test_bptree_basic(void) {
    BPlusTree tree;
    char error_buf[256];
    KeyCollector collector;
    long value = 0;

    bptree_init(&tree);
    ensure_directory("test_data");
    remove_file_if_exists("test_data/tree.idx");
    expect(bptree_open(&tree, "test_data/tree.idx", error_buf, sizeof(error_buf)), "B+ tree open failed");
    expect(bptree_insert(&tree, 10, 100, error_buf, sizeof(error_buf)), "B+ tree insert 10 failed");
    expect(bptree_insert(&tree, 20, 200, error_buf, sizeof(error_buf)), "B+ tree insert 20 failed");
    expect(bptree_insert(&tree, 5, 50, error_buf, sizeof(error_buf)), "B+ tree insert 5 failed");
    expect(bptree_insert(&tree, 30, 300, error_buf, sizeof(error_buf)), "B+ tree insert 30 failed");
    expect(bptree_insert(&tree, 25, 250, error_buf, sizeof(error_buf)), "B+ tree insert 25 failed");
    expect(bptree_insert(&tree, 15, 150, error_buf, sizeof(error_buf)), "B+ tree insert 15 failed");

    expect(bptree_search(&tree, 25, &value, error_buf, sizeof(error_buf)) && value == 250, "B+ tree search should find key 25");

    memset(&collector, 0, sizeof(collector));
    expect(bptree_visit(&tree,
                        COMPARE_GREATER_THAN_OR_EQUAL,
                        15,
                        collect_keys,
                        &collector,
                        error_buf,
                        sizeof(error_buf)),
           "B+ tree range visit failed");
    expect(collector.count == 4, "B+ tree >= range count mismatch");
    expect(collector.keys[0] == 15 && collector.keys[3] == 30, "B+ tree >= range order mismatch");

    memset(&collector, 0, sizeof(collector));
    expect(bptree_visit(&tree,
                        COMPARE_NOT_EQUALS,
                        20,
                        collect_keys,
                        &collector,
                        error_buf,
                        sizeof(error_buf)),
           "B+ tree != visit failed");
    expect(collector.count == 5, "B+ tree != count mismatch");

    bptree_destroy(&tree);
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
    fail("테스트 디렉터리 생성에 실패했습니다.");
}

static void write_file(const char *path, const char *contents) {
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        fail("테스트 파일 생성에 실패했습니다.");
    }
    fputs(contents, file);
    fclose(file);
}

static char *read_file(const char *path) {
    FILE *file = fopen(path, "rb");
    long length;
    char *buffer;

    if (file == NULL) {
        fail("테스트 결과 파일을 읽을 수 없습니다.");
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        fail("파일 길이를 읽을 수 없습니다.");
    }

    length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        fail("파일 포인터 이동에 실패했습니다.");
    }

    buffer = (char *)malloc((size_t)length + 1);
    if (buffer == NULL) {
        fclose(file);
        fail("메모리 할당에 실패했습니다.");
    }

    fread(buffer, 1, (size_t)length, file);
    buffer[length] = '\0';
    fclose(file);
    return buffer;
}

static void test_engine_index_and_insert(void) {
    SqlEngine engine;
    QueryStats stats;
    char error_buf[512];
    char *file_contents;

    ensure_directory("test_data");
    remove_file_if_exists("test_data/users.idx");
    write_file("test_data/users.tbl",
               "id,name,email,age\n"
               "1,Alice,alice@example.com,20\n"
               "2,Bob,bob@example.com,25\n");

    expect(sql_engine_init(&engine, "test_data", error_buf, sizeof(error_buf)), "engine init failed");

    expect(sql_engine_execute_sql(&engine,
                                  "INSERT INTO users (email, name, age) VALUES ('carol@example.com', 'Carol', 30);",
                                  NULL,
                                  &stats,
                                  error_buf,
                                  sizeof(error_buf)),
           error_buf);

    file_contents = read_file("test_data/users.tbl");
    expect(strstr(file_contents, "3,Carol,carol@example.com,30") != NULL, "swapped insert columns should map to schema order");
    free(file_contents);

    expect(sql_engine_execute_sql(&engine,
                                  "SELECT name FROM users WHERE id = 2;",
                                  NULL,
                                  &stats,
                                  error_buf,
                                  sizeof(error_buf)),
           error_buf);
    expect(stats.used_index == 1 && stats.matched_rows == 1, "id = should use index");

    expect(sql_engine_execute_sql(&engine,
                                  "SELECT name FROM users WHERE id >= 2;",
                                  NULL,
                                  &stats,
                                  error_buf,
                                  sizeof(error_buf)),
           error_buf);
    expect(stats.used_index == 1 && stats.matched_rows == 2, "id >= should use index");

    expect(sql_engine_execute_sql(&engine,
                                  "SELECT name FROM users WHERE id < 3;",
                                  NULL,
                                  &stats,
                                  error_buf,
                                  sizeof(error_buf)),
           error_buf);
    expect(stats.used_index == 1 && stats.matched_rows == 2, "id < should use index");

    expect(sql_engine_execute_sql(&engine,
                                  "SELECT name FROM users WHERE id != 2;",
                                  NULL,
                                  &stats,
                                  error_buf,
                                  sizeof(error_buf)),
           error_buf);
    expect(stats.used_index == 1 && stats.matched_rows == 2, "id != should use index");

    expect(sql_engine_execute_sql(&engine,
                                  "SELECT id FROM users WHERE email = 'bob@example.com';",
                                  NULL,
                                  &stats,
                                  error_buf,
                                  sizeof(error_buf)),
           error_buf);
    expect(stats.used_index == 0 && stats.matched_rows == 1, "non-id filter should stay linear");

    sql_engine_free(&engine);
}

int main(void) {
    test_bptree_basic();
    test_engine_index_and_insert();
    printf("[PASS] mini_sql tests completed\n");
    return 0;
}
