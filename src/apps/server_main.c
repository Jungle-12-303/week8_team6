#include "api_server.h"

/*
 * ============================================================================
 * [Server Main 코드 리뷰용 흐름 지도]
 * ============================================================================
 *
 * 이 파일의 역할:
 * - 실행 옵션을 읽어 ApiServerConfig 를 만든다.
 * - 실제 네트워크/DB 처리는 api_server_run() 에 위임한다.
 *
 * 실행 예:
 *   ./mini_sql_server --data-dir runtime_data --host 127.0.0.1 --port 8080 --threads 4
 *
 * 전체 호출 흐름:
 *
 * main()
 *      |
 *      +--> 기본값 설정
 *      |       host=0.0.0.0, port=8080, threads=4, queue=64
 *      |
 *      +--> argv 순회
 *      |       --data-dir, --host, --port, --threads, --queue, --max-body 반영
 *      |
 *      +--> api_server_run(&config, ...)
 *              실제 서버 시작
 *
 * 분기:
 * - --help: 사용법 출력 후 정상 종료
 * - 잘못된 옵션/숫자: 에러 출력 후 종료
 * - api_server_run 실패: 원인 출력 후 종료
 * ============================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_HOST "0.0.0.0"
#define DEFAULT_PORT 8080
#define DEFAULT_THREADS 4
#define DEFAULT_QUEUE_CAPACITY 64
#define DEFAULT_MAX_BODY_BYTES 65536

/* 사용자가 --help 를 입력했을 때 API 사용법을 바로 볼 수 있게 한다. */
static void print_usage(const char *program) {
    printf("Usage: %s [--data-dir DIR] [--host HOST] [--port PORT] [--threads N] [--queue N] [--max-body BYTES]\n",
           program);
    printf("\n");
    printf("Endpoints:\n");
    printf("  GET  /health\n");
    printf("  POST /query    body: raw SQL or {\"sql\":\"SELECT * FROM users;\"}\n");
    printf("  GET  /metrics\n");
}

/*
 * port 처럼 int 범위의 양수 옵션을 검증한다.
 * 문자열 안에 숫자 외 문자가 섞이면 실패 처리한다.
 */
static int parse_int_arg(const char *value, int *out) {
    char *end;
    long parsed = strtol(value, &end, 10);

    if (value[0] == '\0' || *end != '\0' || parsed <= 0 || parsed > 2147483647L) {
        return 0;
    }

    *out = (int)parsed;
    return 1;
}

/*
 * thread 수, queue 크기, max-body 처럼 size_t 로 저장할 양수 옵션을 검증한다.
 */
static int parse_size_arg(const char *value, size_t *out) {
    char *end;
    unsigned long parsed = strtoul(value, &end, 10);

    if (value[0] == '\0' || *end != '\0' || parsed == 0) {
        return 0;
    }

    *out = (size_t)parsed;
    return 1;
}

/*
 * 프로그램 진입점.
 * 여기서는 설정만 만들고, 서버 수명 주기는 api_server_run() 이 책임진다.
 */
int main(int argc, char **argv) {
    ApiServerConfig config;
    char error[512];
    const char *env_data_dir;
    int index;

    env_data_dir = getenv("MINI_SQL_DATA_DIR");

    memset(&config, 0, sizeof(config));
    config.host = DEFAULT_HOST;
    config.port = DEFAULT_PORT;
    config.data_dir = (env_data_dir != NULL && env_data_dir[0] != '\0') ? env_data_dir : "data";
    config.thread_count = DEFAULT_THREADS;
    config.queue_capacity = DEFAULT_QUEUE_CAPACITY;
    config.max_body_bytes = DEFAULT_MAX_BODY_BYTES;

    for (index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--help") == 0 || strcmp(argv[index], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[index], "--data-dir") == 0 && index + 1 < argc) {
            config.data_dir = argv[++index];
        } else if (strcmp(argv[index], "--host") == 0 && index + 1 < argc) {
            config.host = argv[++index];
        } else if (strcmp(argv[index], "--port") == 0 && index + 1 < argc) {
            if (!parse_int_arg(argv[++index], &config.port)) {
                fprintf(stderr, "Invalid --port value\n");
                return 1;
            }
        } else if (strcmp(argv[index], "--threads") == 0 && index + 1 < argc) {
            if (!parse_size_arg(argv[++index], &config.thread_count)) {
                fprintf(stderr, "Invalid --threads value\n");
                return 1;
            }
        } else if (strcmp(argv[index], "--queue") == 0 && index + 1 < argc) {
            if (!parse_size_arg(argv[++index], &config.queue_capacity)) {
                fprintf(stderr, "Invalid --queue value\n");
                return 1;
            }
        } else if (strcmp(argv[index], "--max-body") == 0 && index + 1 < argc) {
            if (!parse_size_arg(argv[++index], &config.max_body_bytes)) {
                fprintf(stderr, "Invalid --max-body value\n");
                return 1;
            }
        } else {
            fprintf(stderr, "Unknown or incomplete option: %s\n", argv[index]);
            print_usage(argv[0]);
            return 1;
        }
    }

    memset(error, 0, sizeof(error));
    if (!api_server_run(&config, error, sizeof(error))) {
        fprintf(stderr, "API server failed: %s\n", error);
        return 1;
    }

    return 0;
}
