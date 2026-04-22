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

#define SPECIAL_NAME "CHOIHYUNJIN"
#define SPECIAL_EMAIL "guswls1478@gmail.com"
#define SPECIAL_AGE 24

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
        fprintf(stderr, "시드 테이블을 만들 수 없습니다: %s\n", path);
        exit(1);
    }

    fputs("id,name,email,age\n", file);
    fclose(file);

    index_file = fopen(index_path, "wb");
    if (index_file == NULL) {
        fprintf(stderr, "시드 인덱스 파일을 초기화할 수 없습니다: %s\n", index_path);
        exit(1);
    }
    fclose(index_file);
}

static int count_existing_rows(const char *data_dir, const char *table_name) {
    char path[1024];
    FILE *file;
    char line[8192];
    int line_count = 0;

    snprintf(path, sizeof(path), "%s/%s.tbl", data_dir, table_name);
    file = fopen(path, "rb");
    if (file == NULL) {
        return -1;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        ++line_count;
    }
    fclose(file);

    if (line_count <= 0) {
        return 0;
    }

    return line_count - 1;
}

static void reset_index_file(const char *data_dir, const char *table_name) {
    char index_path[1024];
    FILE *index_file;

    snprintf(index_path, sizeof(index_path), "%s/%s.idx", data_dir, table_name);
    index_file = fopen(index_path, "wb");
    if (index_file == NULL) {
        fprintf(stderr, "시드 인덱스 파일을 재설정할 수 없습니다: %s\n", index_path);
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

int main(int argc, char **argv) {
    const char *data_dir = "generated_data";
    const char *table_name = "users";
    int rows = 1000000;
    int special_row_id = 777777;
    int resume_mode = 0;
    int existing_rows = 0;
    int start_row = 1;
    int progress_step = 100000;
    SqlEngine engine;
    char error_buf[512];
    char sql[512];
    char random_name[64];
    char random_email[128];
    int random_age;
    int index;

    memset(&engine, 0, sizeof(engine));
    memset(error_buf, 0, sizeof(error_buf));

    for (index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--rows") == 0 && index + 1 < argc) {
            rows = atoi(argv[++index]);
        } else if (strcmp(argv[index], "--data-dir") == 0 && index + 1 < argc) {
            data_dir = argv[++index];
        } else if (strcmp(argv[index], "--special-row-id") == 0 && index + 1 < argc) {
            special_row_id = atoi(argv[++index]);
        } else if (strcmp(argv[index], "--resume") == 0) {
            resume_mode = 1;
        }
    }

    if (rows <= 0) {
        fprintf(stderr, "--rows 는 1 이상이어야 합니다.\n");
        return 1;
    }

    if (special_row_id < 1 || special_row_id > rows) {
        special_row_id = rows >= 777777 ? 777777 : rows;
    }

    disable_query_timing_log();
    srand((unsigned int)time(NULL));
    if (resume_mode) {
        ensure_directory(data_dir);
        existing_rows = count_existing_rows(data_dir, table_name);
        if (existing_rows < 0) {
            fprintf(stderr, "resume 대상 테이블이 없어 새 시드를 시작합니다.\n");
            prepare_table(data_dir, table_name);
            existing_rows = 0;
        } else {
            reset_index_file(data_dir, table_name);
            start_row = existing_rows + 1;
            printf("Resume mode: existing_rows=%d start_row=%d target_rows=%d\n", existing_rows, start_row, rows);
        }
    } else {
        prepare_table(data_dir, table_name);
    }

    if (existing_rows >= rows) {
        printf("Seed already complete: existing_rows=%d target_rows=%d\n", existing_rows, rows);
        return 0;
    }

    if (!sql_engine_init(&engine, data_dir, error_buf, sizeof(error_buf))) {
        fprintf(stderr, "초기화 실패: %s\n", error_buf);
        return 1;
    }

    if (rows >= 100) {
        progress_step = rows / 100;
    } else if (rows > 0) {
        progress_step = 1;
    }

    for (index = start_row; index <= rows; ++index) {
        if (index == special_row_id) {
            snprintf(sql,
                     sizeof(sql),
                     "INSERT INTO %s (name, email, age) VALUES ('%s', '%s', %d);",
                     table_name,
                     SPECIAL_NAME,
                     SPECIAL_EMAIL,
                     SPECIAL_AGE);
        } else {
            generate_random_name(random_name, sizeof(random_name));
            generate_random_email(random_email, sizeof(random_email), index);
            random_age = random_int(18, 60);

            snprintf(sql,
                     sizeof(sql),
                     "INSERT INTO %s (name, email, age) VALUES ('%s', '%s', %d);",
                     table_name,
                     random_name,
                     random_email,
                     random_age);
        }

        if (!sql_engine_execute_sql(&engine, sql, NULL, NULL, error_buf, sizeof(error_buf))) {
            fprintf(stderr, "INSERT 실패 (%d): %s\n", index, error_buf);
            sql_engine_free(&engine);
            return 1;
        }

        if (progress_step > 0 && (index % progress_step == 0 || index == rows)) {
            printf("Seeded %d rows (%d%%)...\n", index, (index * 100) / rows);
        }
    }

    printf("Seed complete: %d rows\n", rows);
    printf("Special row id: %d\n", special_row_id);
    printf("Special row: name=%s email=%s age=%d\n", SPECIAL_NAME, SPECIAL_EMAIL, SPECIAL_AGE);

    sql_engine_free(&engine);
    return 0;
}
