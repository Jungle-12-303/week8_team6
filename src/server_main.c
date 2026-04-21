#include "api_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_HOST "0.0.0.0"
#define DEFAULT_PORT 8080
#define DEFAULT_THREADS 4
#define DEFAULT_QUEUE_CAPACITY 64
#define DEFAULT_MAX_BODY_BYTES 65536

static void print_usage(const char *program) {
    printf("Usage: %s [--data-dir DIR] [--host HOST] [--port PORT] [--threads N] [--queue N] [--max-body BYTES]\n",
           program);
    printf("\n");
    printf("Endpoints:\n");
    printf("  GET  /health\n");
    printf("  POST /query    body: raw SQL or {\"sql\":\"SELECT * FROM users;\"}\n");
    printf("  GET  /metrics\n");
}

static int parse_int_arg(const char *value, int *out) {
    char *end;
    long parsed = strtol(value, &end, 10);

    if (value[0] == '\0' || *end != '\0' || parsed <= 0 || parsed > 2147483647L) {
        return 0;
    }

    *out = (int)parsed;
    return 1;
}

static int parse_size_arg(const char *value, size_t *out) {
    char *end;
    unsigned long parsed = strtoul(value, &end, 10);

    if (value[0] == '\0' || *end != '\0' || parsed == 0) {
        return 0;
    }

    *out = (size_t)parsed;
    return 1;
}

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
