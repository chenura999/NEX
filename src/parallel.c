/*
 * NEX Compress — Parallel Processing Module
 * Thread pool with work queue for chunk compression
 */

#include "nex_internal.h"
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>

/* ── Work Item ───────────────────────────────────────────────────── */

typedef struct nex_work_item {
    nex_task_fn           fn;
    void                 *arg;
    struct nex_work_item *next;
} nex_work_item_t;

/* ── Thread Pool ─────────────────────────────────────────────────── */

struct nex_thread_pool {
    pthread_t      *threads;
    int             num_threads;

    /* Work queue */
    nex_work_item_t *queue_head;
    nex_work_item_t *queue_tail;
    pthread_mutex_t  queue_mutex;
    pthread_cond_t   queue_cond;

    /* Completion tracking */
    atomic_int       pending;
    pthread_mutex_t  done_mutex;
    pthread_cond_t   done_cond;

    /* Shutdown flag */
    volatile bool    shutdown;
};

/* ── Worker Thread ───────────────────────────────────────────────── */

static void *pool_worker(void *arg) {
    nex_thread_pool_t *pool = (nex_thread_pool_t *)arg;

    while (1) {
        pthread_mutex_lock(&pool->queue_mutex);

        while (!pool->queue_head && !pool->shutdown) {
            pthread_cond_wait(&pool->queue_cond, &pool->queue_mutex);
        }

        if (pool->shutdown && !pool->queue_head) {
            pthread_mutex_unlock(&pool->queue_mutex);
            break;
        }

        /* Dequeue work item */
        nex_work_item_t *item = pool->queue_head;
        pool->queue_head = item->next;
        if (!pool->queue_head) pool->queue_tail = NULL;

        pthread_mutex_unlock(&pool->queue_mutex);

        /* Execute task */
        item->fn(item->arg);
        free(item);

        /* Signal completion */
        if (atomic_fetch_sub(&pool->pending, 1) == 1) {
            pthread_mutex_lock(&pool->done_mutex);
            pthread_cond_broadcast(&pool->done_cond);
            pthread_mutex_unlock(&pool->done_mutex);
        }
    }

    return NULL;
}

/* ── Pool Create ─────────────────────────────────────────────────── */

nex_thread_pool_t *nex_pool_create(int num_threads) {
    if (num_threads <= 0) {
        /* Auto-detect: use number of online CPUs */
        long ncpus = sysconf(_SC_NPROCESSORS_ONLN);
        num_threads = (ncpus > 0) ? (int)ncpus : 4;
    }

    nex_thread_pool_t *pool = (nex_thread_pool_t *)calloc(1, sizeof(*pool));
    if (!pool) return NULL;

    pool->num_threads = num_threads;
    pool->queue_head = NULL;
    pool->queue_tail = NULL;
    pool->shutdown = false;
    atomic_store(&pool->pending, 0);

    pthread_mutex_init(&pool->queue_mutex, NULL);
    pthread_cond_init(&pool->queue_cond, NULL);
    pthread_mutex_init(&pool->done_mutex, NULL);
    pthread_cond_init(&pool->done_cond, NULL);

    pool->threads = (pthread_t *)malloc(num_threads * sizeof(pthread_t));
    if (!pool->threads) {
        free(pool);
        return NULL;
    }

    for (int i = 0; i < num_threads; i++) {
        if (pthread_create(&pool->threads[i], NULL, pool_worker, pool) != 0) {
            /* Partial creation: shut down what we have */
            pool->num_threads = i;
            nex_pool_destroy(pool);
            return NULL;
        }
    }

    return pool;
}

/* ── Pool Submit ─────────────────────────────────────────────────── */

nex_status_t nex_pool_submit(nex_thread_pool_t *pool, nex_task_fn fn,
                              void *arg) {
    nex_work_item_t *item = (nex_work_item_t *)malloc(sizeof(*item));
    if (!item) return NEX_ERR_NOMEM;

    item->fn = fn;
    item->arg = arg;
    item->next = NULL;

    atomic_fetch_add(&pool->pending, 1);

    pthread_mutex_lock(&pool->queue_mutex);
    if (pool->queue_tail) {
        pool->queue_tail->next = item;
    } else {
        pool->queue_head = item;
    }
    pool->queue_tail = item;
    pthread_cond_signal(&pool->queue_cond);
    pthread_mutex_unlock(&pool->queue_mutex);

    return NEX_OK;
}

/* ── Pool Wait ───────────────────────────────────────────────────── */

void nex_pool_wait(nex_thread_pool_t *pool) {
    pthread_mutex_lock(&pool->done_mutex);
    while (atomic_load(&pool->pending) > 0) {
        pthread_cond_wait(&pool->done_cond, &pool->done_mutex);
    }
    pthread_mutex_unlock(&pool->done_mutex);
}

/* ── Pool Destroy ────────────────────────────────────────────────── */

void nex_pool_destroy(nex_thread_pool_t *pool) {
    if (!pool) return;

    /* Signal shutdown */
    pthread_mutex_lock(&pool->queue_mutex);
    pool->shutdown = true;
    pthread_cond_broadcast(&pool->queue_cond);
    pthread_mutex_unlock(&pool->queue_mutex);

    /* Join all threads */
    for (int i = 0; i < pool->num_threads; i++) {
        pthread_join(pool->threads[i], NULL);
    }

    /* Free remaining queue items */
    nex_work_item_t *item = pool->queue_head;
    while (item) {
        nex_work_item_t *next = item->next;
        free(item);
        item = next;
    }

    pthread_mutex_destroy(&pool->queue_mutex);
    pthread_cond_destroy(&pool->queue_cond);
    pthread_mutex_destroy(&pool->done_mutex);
    pthread_cond_destroy(&pool->done_cond);

    free(pool->threads);
    free(pool);
}
