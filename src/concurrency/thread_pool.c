#include "thread_pool.h"

/*
 * ============================================================================
 * [Thread Pool 코드 리뷰용 흐름 지도]
 * ============================================================================
 *
 * 이 파일의 역할:
 * - 서버 시작 시 고정 개수의 worker thread 를 만들어 둔다.
 * - accept loop 에서 들어온 client 작업을 queue 에 넣는다.
 * - 쉬고 있던 worker 가 queue 에서 작업을 꺼내 handle_client() 를 실행한다.
 *
 * 전체 호출 흐름:
 *
 * api_server_run()
 *      |
 *      +--> thread_pool_create()
 *      |       worker thread N개 생성
 *      |       각 worker 는 worker_main() 에서 작업 대기
 *      |
 *      +--> accept() 로 클라이언트 연결 수락
 *      |
 *      +--> thread_pool_submit(handle_client, task)
 *              queue tail 에 작업 push
 *              not_empty signal 로 worker 깨움
 *
 * worker_main()
 *      |
 *      +--> queue 가 비어 있으면 cond_wait()
 *      +--> 작업이 들어오면 head 에서 pop
 *      +--> mutex 를 풀고 task.function(task.arg) 실행
 *
 * 종료 흐름:
 * thread_pool_destroy()
 *      |
 *      +--> stopping = 1
 *      +--> 모든 worker 깨움
 *      +--> 이미 queue 에 들어온 작업은 처리 후 종료
 *      +--> thread join 후 메모리 해제
 *
 * 왜 필요한가:
 * 클라이언트 요청마다 thread 를 새로 만들면 비용이 크고 제어가 어렵다.
 * thread pool 은 정해진 worker 수 안에서 요청을 병렬 처리하고, queue 로 과부하를 흡수한다.
 * ============================================================================
 */

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

/*
 * worker thread 의 main loop.
 *
 * 핵심 규칙:
 * - queue 확인/수정은 mutex 안에서만 한다.
 * - 실제 요청 처리(task.function)는 mutex 밖에서 한다.
 *
 * 두 번째 규칙이 중요하다. handle_client() 가 DB/API 작업을 하는 동안
 * queue lock 을 잡고 있으면 다른 worker 가 다음 작업을 못 꺼낸다.
 */
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

/*
 * 생성 중 실패했거나 destroy 마지막 단계에서 pool 이 가진 heap 메모리를 정리한다.
 * OS thread join/condition/mutex 정리는 caller 쪽에서 끝낸 뒤 호출한다.
 */
static void cleanup_pool(ThreadPool *pool) {
    if (pool == NULL) {
        return;
    }

    free(pool->threads);
    free(pool->queue);
    free(pool);
}

/*
 * thread pool 을 만든다.
 *
 * worker_count:
 * - 동시에 HTTP 요청을 처리할 worker 수
 *
 * queue_capacity:
 * - worker 가 모두 바쁠 때 잠시 쌓아둘 요청 수
 *
 * 실패 시:
 * - 이미 만든 thread/메모리를 thread_pool_destroy() 또는 cleanup_pool() 로 정리한다.
 */
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

/*
 * accept loop 가 client 작업을 queue 에 넣을 때 호출한다.
 *
 * 분기:
 * - queue 에 빈 칸 있음: 바로 tail 에 push
 * - queue 가 가득 참: not_full 신호가 올 때까지 대기
 * - stopping 상태: 더 이상 새 작업을 받지 않고 실패 반환
 */
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

/*
 * 서버 종료 시 worker 들을 안전하게 멈춘다.
 *
 * 이 구현은 "이미 queue 에 들어간 작업"을 버리지 않는다.
 * stopping 을 켠 뒤 worker 를 모두 깨우고, worker 가 남은 작업을 비운 뒤 종료하게 한다.
 */
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
