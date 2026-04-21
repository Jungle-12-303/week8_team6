#ifndef API_SERVER_H
#define API_SERVER_H

#include <stddef.h>

typedef struct {
    /* Bind address. Use 127.0.0.1 for local Windows demo, 0.0.0.0 for Docker. */
    const char *host;

    /* TCP port exposed to Thunder Client. */
    int port;

    /* Directory containing users.tbl and generated users.idx. */
    const char *data_dir;

    /* Number of worker threads waiting on the request queue. */
    size_t thread_count;

    /* Maximum queued client sockets before accept starts applying backpressure. */
    size_t queue_capacity;

    /* Maximum accepted HTTP request body size, protecting the server from huge SQL bodies. */
    size_t max_body_bytes;
} ApiServerConfig;

/*
 * Blocking server loop. The process is normally stopped with Ctrl+C during demos.
 * Returns 0 only if listener setup or accept fails.
 */
int api_server_run(const ApiServerConfig *config, char *error_buf, size_t error_buf_size);

#endif
