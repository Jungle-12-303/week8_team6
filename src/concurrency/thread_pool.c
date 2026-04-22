#include "thread_pool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <windows.h>
typedef HANDLE ThreadHandle;
typedef CRITICAL_SECTION ThreadMutex;
typedef CONDITION_VARIABLE ThreadCond;
#define THREAD_RET DWORD WINAPI

static int mutex_init(ThreadMutex *mutex) {
    InitializeCriticalSection(mutex);
    return 1;
}

static void mutex_destroy(ThreadMutex *mutex) {
    DeleteCriticalSection(mutex);
}

static void mutex_lock(ThreadMutex *mutex) {
    EnterCriticalSection(mutex);
}

static void mutex_unlock(ThreadMutex *mutex) {
    LeaveCriticalSection(mutex);
}

static void cond_init(ThreadCond *cond) {
    InitializeConditionVariable(cond);
}

static void cond_wait(ThreadCond *cond, ThreadMutex *mutex) {
    SleepConditionVariableCS(cond, mutex, INFINITE);
}

static void cond_signal(ThreadCond *cond) {
    WakeConditionVariable(cond);
}

static void cond_broadcast(ThreadCond *cond) {
    WakeAllConditionVariable(cond);
}
#else
#include <pthread.h>
typedef pthread_t ThreadHandle;
typedef pthread_mutex_t ThreadMutex;
typedef pthread_cond_t ThreadCond;
#define THREAD_RET void *

static int mutex_init(ThreadMutex *mutex) {
    return pthread_mutex_init(mutex, NULL) == 0;
}

static void mutex_destroy(ThreadMutex *mutex) {
    pthread_mutex_destroy(mutex);
}

static void mutex_lock(ThreadMutex *mutex) {
    pthread_mutex_lock(mutex);
}

static void mutex_unlock(ThreadMutex *mutex) {
    pthread_mutex_unlock(mutex);
}

static void cond_init(ThreadCond *cond) {
    pthread_cond_init(cond, NULL);
}

static void cond_wait(ThreadCond *cond, ThreadMutex *mutex) {
    pthread_cond_wait(cond, mutex);
}

static void cond_signal(ThreadCond *cond) {
    pthread_cond_signal(cond);
}

static void cond_broadcast(ThreadCond *cond) {
    pthread_cond_broadcast(cond);
}

static void cond_destroy(ThreadCond *cond) {
    pthread_cond_destroy(cond);
}
#endif

typedef struct {
    ThreadPoolTaskFn function;
    void *arg;
} ThreadPoolTask;

struct ThreadPool {
    /* OS thread handles kept so destroy can wait for every worker. */
    ThreadHandle *threads;
    size_t worker_count;

    /* Circular queue: head pops work, tail pushes work. */
    ThreadPoolTask *queue;
    size_t queue_capacity;
    size_t head;
    size_t tail;
    size_t count;

    /* Once stopping is set, submit fails and workers exit after queued work. */
    int stopping;

    /* Mutex protects queue indexes, queue count, and stopping flag. */
    ThreadMutex mutex;

    /* not_empty wakes workers; not_full wakes submitters blocked by backpressure. */
    ThreadCond not_empty;
    ThreadCond not_full;
};

static THREAD_RET worker_main(void *arg) {
    ThreadPool *pool = (ThreadPool *)arg;

    while (1) {
        ThreadPoolTask task;

        mutex_lock(&pool->mutex);
        /* Workers sleep without burning CPU while no client work exists. */
        while (!pool->stopping && pool->count == 0) {
            cond_wait(&pool->not_empty, &pool->mutex);
        }

        /* Shutdown finishes only after all already-queued tasks are handled. */
        if (pool->stopping && pool->count == 0) {
            mutex_unlock(&pool->mutex);
            break;
        }

        /* Pop one task from the circular queue while holding the lock. */
        task = pool->queue[pool->head];
        pool->head = (pool->head + 1) % pool->queue_capacity;
        --pool->count;
        cond_signal(&pool->not_full);
        mutex_unlock(&pool->mutex);

        /* Run user code outside the queue lock so other workers can progress. */
        task.function(task.arg);
    }

#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

static void cleanup_pool(ThreadPool *pool) {
    if (pool == NULL) {
        return;
    }

    free(pool->threads);
    free(pool->queue);
    free(pool);
}

ThreadPool *thread_pool_create(size_t worker_count,
                               size_t queue_capacity,
                               char *error_buf,
                               size_t error_buf_size) {
    ThreadPool *pool;
    size_t index;

    if (worker_count == 0) {
        worker_count = 1;
    }
    if (queue_capacity == 0) {
        queue_capacity = worker_count * 4;
    }

    pool = (ThreadPool *)calloc(1, sizeof(*pool));
    if (pool == NULL) {
        snprintf(error_buf, error_buf_size, "failed to allocate thread pool");
        return NULL;
    }

    pool->threads = (ThreadHandle *)calloc(worker_count, sizeof(ThreadHandle));
    pool->queue = (ThreadPoolTask *)calloc(queue_capacity, sizeof(ThreadPoolTask));
    if (pool->threads == NULL || pool->queue == NULL) {
        snprintf(error_buf, error_buf_size, "failed to allocate thread pool arrays");
        cleanup_pool(pool);
        return NULL;
    }

    pool->worker_count = worker_count;
    pool->queue_capacity = queue_capacity;
    if (!mutex_init(&pool->mutex)) {
        snprintf(error_buf, error_buf_size, "failed to initialize thread pool mutex");
        cleanup_pool(pool);
        return NULL;
    }
    cond_init(&pool->not_empty);
    cond_init(&pool->not_full);

    for (index = 0; index < worker_count; ++index) {
#if defined(_WIN32)
        pool->threads[index] = CreateThread(NULL, 0, worker_main, pool, 0, NULL);
        if (pool->threads[index] == NULL) {
#else
        if (pthread_create(&pool->threads[index], NULL, worker_main, pool) != 0) {
#endif
            snprintf(error_buf, error_buf_size, "failed to create worker thread");
            pool->worker_count = index;
            thread_pool_destroy(pool);
            return NULL;
        }
    }

    return pool;
}

int thread_pool_submit(ThreadPool *pool, ThreadPoolTaskFn function, void *arg) {
    if (pool == NULL || function == NULL) {
        return 0;
    }

    mutex_lock(&pool->mutex);

    /*
     * Bounded queue behavior:
     * If all workers are busy and the queue is full, the accept loop waits here.
     * This keeps memory usage predictable under many client requests.
     */
    while (!pool->stopping && pool->count == pool->queue_capacity) {
        cond_wait(&pool->not_full, &pool->mutex);
    }

    if (pool->stopping) {
        mutex_unlock(&pool->mutex);
        return 0;
    }

    /* Push one task at tail, then wake a sleeping worker. */
    pool->queue[pool->tail].function = function;
    pool->queue[pool->tail].arg = arg;
    pool->tail = (pool->tail + 1) % pool->queue_capacity;
    ++pool->count;
    cond_signal(&pool->not_empty);
    mutex_unlock(&pool->mutex);
    return 1;
}

void thread_pool_destroy(ThreadPool *pool) {
    size_t index;

    if (pool == NULL) {
        return;
    }

    mutex_lock(&pool->mutex);
    pool->stopping = 1;
    cond_broadcast(&pool->not_empty);
    cond_broadcast(&pool->not_full);
    mutex_unlock(&pool->mutex);

    for (index = 0; index < pool->worker_count; ++index) {
#if defined(_WIN32)
        if (pool->threads[index] != NULL) {
            WaitForSingleObject(pool->threads[index], INFINITE);
            CloseHandle(pool->threads[index]);
        }
#else
        pthread_join(pool->threads[index], NULL);
#endif
    }

#if !defined(_WIN32)
    cond_destroy(&pool->not_empty);
    cond_destroy(&pool->not_full);
#endif
    mutex_destroy(&pool->mutex);
    cleanup_pool(pool);
}
