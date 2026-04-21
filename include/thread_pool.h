#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <stddef.h>

typedef void (*ThreadPoolTaskFn)(void *arg);

typedef struct ThreadPool ThreadPool;

/*
 * Creates fixed worker threads and a bounded task queue.
 * Requests are submitted to the queue instead of creating a new thread per client.
 */
ThreadPool *thread_pool_create(size_t worker_count,
                               size_t queue_capacity,
                               char *error_buf,
                               size_t error_buf_size);

/*
 * Adds one unit of work to the queue. The call waits while the queue is full,
 * which applies simple backpressure to the accept loop.
 */
int thread_pool_submit(ThreadPool *pool, ThreadPoolTaskFn function, void *arg);

/*
 * Stops accepting new tasks, wakes workers, waits for queued work to finish,
 * then releases OS thread and queue resources.
 */
void thread_pool_destroy(ThreadPool *pool);

#endif
