#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200112L
#endif

#include "engine.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <direct.h>
#include <stdlib.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

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

    fprintf(stderr, "디렉터리를 생성할 수 없습니다: %s\n", path);
    exit(1);
}

static int random_int(int min_inclusive, int max_inclusive) {
    if (max_inclusive <= min_inclusive) {
        return min_inclusive;
    }

    return min_inclusive + rand() % (max_inclusive - min_inclusive + 1);
}

static void random_letters(char *buffer, size_t length, int uppercase_first) {
    static const char lower[] = "abcdefghijklmnopqrstuvwxyz";
    size_t index;

    if (length == 0) {
        return;
    }

    for (index = 0; index + 1 < length; ++index) {
        char letter = lower[rand() % (sizeof(lower) - 1)];
        if (index == 0 && uppercase_first) {
            letter = (char)(letter - ('a' - 'A'));
        }
        buffer[index] = letter;
    }
    buffer[length - 1] = '\0';
}

static void random_alnum(char *buffer, size_t length) {
    static const char chars[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    size_t index;

    if (length == 0) {
        return;
    }

    for (index = 0; index + 1 < length; ++index) {
        buffer[index] = chars[rand() % (sizeof(chars) - 1)];
    }
    buffer[length - 1] = '\0';
}

static void generate_random_name(char *buffer, size_t buffer_size) {
    char first[16];
    char last[20];

    random_letters(first, (size_t)random_int(5, 9), 1);
    random_letters(last, (size_t)random_int(6, 11), 1);
    snprintf(buffer, buffer_size, "%s%s", first, last);
}

static void generate_random_email(char *buffer, size_t buffer_size, int row_id) {
    static const char *domains[] = {
        "example.com",
        "mail.net",
        "demo.org",
        "sample.dev"
    };
    char local_part[20];
    const char *domain = domains[rand() % (sizeof(domains) / sizeof(domains[0]))];

    random_alnum(local_part, (size_t)random_int(9, 15));
    snprintf(buffer, buffer_size, "%s-%07d@%s", local_part, row_id, domain);
}

static void prepare_table(const char *data_dir, const char *table_name) {
    char path[1024];
    char index_path[1024];
    FILE *file;
    FILE *index_file;

    ensure_directory(data_dir);
    snprintf(path, sizeof(path), "%s/%s.tbl", data_dir, table_name);
    snprintf(index_path, sizeof(index_path), "%s/%s.idx", data_dir, table_name);
    remove(path);
    remove(index_path);

    file = fopen(path, "wb");
    if (file == NULL) {
        fprintf(stderr, "벤치마크 테이블을 만들 수 없습니다: %s\n", path);
        exit(1);
    }

    fputs("id,name,email,age\n", file);
    fclose(file);

    index_file = fopen(index_path, "wb");
    if (index_file == NULL) {
        fprintf(stderr, "벤치마크 인덱스 파일을 초기화할 수 없습니다: %s\n", index_path);
        exit(1);
    }
    fclose(index_file);
}

static void disable_query_timing_log(void) {
#if defined(_WIN32)
    _putenv("MINI_SQL_DISABLE_QUERY_LOG=1");
#else
    setenv("MINI_SQL_DISABLE_QUERY_LOG", "1", 1);
#endif
}

static double run_query_average(SqlEngine *engine,
                                const char *sql,
                                int repetitions,
                                QueryStats *last_stats,
                                char *error_buf,
                                size_t error_buf_size) {
    int index;
    double total_ms = 0.0;

    for (index = 0; index < repetitions; ++index) {
        clock_t start = clock();
        if (!sql_engine_execute_sql(engine, sql, NULL, last_stats, error_buf, error_buf_size)) {
            fprintf(stderr, "쿼리 실행 실패: %s\nSQL: %s\n", error_buf, sql);
            exit(1);
        }
        total_ms += (double)(clock() - start) * 1000.0 / CLOCKS_PER_SEC;
    }

    return total_ms / repetitions;
}

int main(int argc, char **argv) {
    const char *data_dir = "benchmark_data";
    const char *table_name = "users";
    int rows = 1000000;
    int repetitions = 5;
    SqlEngine engine;
    QueryStats stats;
    char error_buf[512];
    char sql[512];
    char random_name[64];
    char random_email[128];
    char sampled_email[128];
    int random_age;
    int index;
    int equality_target_id;
    int range_target_id;
    int sampled_email_row;
    clock_t insert_start;
    double insert_ms;

    memset(&engine, 0, sizeof(engine));
    memset(error_buf, 0, sizeof(error_buf));
    disable_query_timing_log();
    memset(sampled_email, 0, sizeof(sampled_email));

    for (index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--rows") == 0 && index + 1 < argc) {
            rows = atoi(argv[++index]);
        } else if (strcmp(argv[index], "--repetitions") == 0 && index + 1 < argc) {
            repetitions = atoi(argv[++index]);
        } else if (strcmp(argv[index], "--data-dir") == 0 && index + 1 < argc) {
            data_dir = argv[++index];
        }
    }

    if (rows <= 0) {
        fprintf(stderr, "--rows 는 1 이상이어야 합니다.\n");
        return 1;
    }
    if (repetitions <= 0) {
        fprintf(stderr, "--repetitions 는 1 이상이어야 합니다.\n");
        return 1;
    }

    srand((unsigned int)time(NULL));
    equality_target_id = random_int(1, rows);
    range_target_id = random_int(1, rows > 10 ? rows - 9 : rows);
    sampled_email_row = random_int(1, rows);

    prepare_table(data_dir, table_name);

    if (!sql_engine_init(&engine, data_dir, error_buf, sizeof(error_buf))) {
        fprintf(stderr, "초기화 실패: %s\n", error_buf);
        return 1;
    }

    insert_start = clock();
    for (index = 1; index <= rows; ++index) {
        generate_random_name(random_name, sizeof(random_name));
        generate_random_email(random_email, sizeof(random_email), index);
        random_age = random_int(18, 60);

        snprintf(sql,
                 sizeof(sql),
                 "INSERT INTO %s (email, name, age) VALUES ('%s', '%s', %d);",
                 table_name,
                 random_email,
                 random_name,
                 random_age);

        if (!sql_engine_execute_sql(&engine, sql, NULL, NULL, error_buf, sizeof(error_buf))) {
            fprintf(stderr, "INSERT 실패 (%d): %s\n", index, error_buf);
            sql_engine_free(&engine);
            return 1;
        }

        if (index == sampled_email_row) {
            snprintf(sampled_email, sizeof(sampled_email), "%s", random_email);
        }

        if (index % 100000 == 0) {
            printf("Inserted %d rows...\n", index);
        }
    }
    insert_ms = (double)(clock() - insert_start) * 1000.0 / CLOCKS_PER_SEC;

    printf("Rows inserted: %d\n", rows);
    printf("Insert elapsed: %.2f ms\n", insert_ms);
    printf("Random equality target id: %d\n", equality_target_id);
    printf("Random range target id: %d\n", range_target_id);
    printf("Random email target row: %d (%s)\n", sampled_email_row, sampled_email);

    snprintf(sql, sizeof(sql), "SELECT id, name, email, age FROM %s WHERE id = %d;", table_name, equality_target_id);
    printf("id equality avg: %.4f ms\n",
           run_query_average(&engine, sql, repetitions, &stats, error_buf, sizeof(error_buf)));
    printf("  used_index=%d scanned_rows=%zu matched_rows=%zu\n", stats.used_index, stats.scanned_rows, stats.matched_rows);

    snprintf(sql, sizeof(sql), "SELECT id, name, email, age FROM %s WHERE id >= %d;", table_name, range_target_id);
    printf("id range avg: %.4f ms\n",
           run_query_average(&engine, sql, repetitions, &stats, error_buf, sizeof(error_buf)));
    printf("  used_index=%d scanned_rows=%zu matched_rows=%zu\n", stats.used_index, stats.scanned_rows, stats.matched_rows);

    snprintf(sql,
             sizeof(sql),
             "SELECT id, name, email, age FROM %s WHERE email = '%s';",
             table_name,
             sampled_email);
    printf("email equality avg: %.4f ms\n",
           run_query_average(&engine, sql, repetitions, &stats, error_buf, sizeof(error_buf)));
    printf("  used_index=%d scanned_rows=%zu matched_rows=%zu\n", stats.used_index, stats.scanned_rows, stats.matched_rows);

    sql_engine_free(&engine);
    return 0;
}
