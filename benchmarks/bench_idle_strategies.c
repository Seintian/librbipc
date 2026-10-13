/**
 * @file bench_idle_strategies.c
 * @brief In-Depth Investigation and Empirical Benchmark of Non-CPU-Burning Wait Strategies:
 *        DPDK rte_ring, LMAX Disruptor, and librbipc.
 *
 * Strategies evaluated:
 *  1. DPDK-BusySpin          - Baseline DPDK rte_ring MPMC tight polling with _mm_pause.
 *  2. DPDK-PauseLoop         - DPDK rte_power_pmd_mgmt pause callback simulation (adaptive pause burst).
 *  3. DPDK-Interrupt-Epoll   - DPDK Rx-interrupt hybrid architecture (eventfd + epoll_wait 10ms).
 *  4. LMAX-BusySpin          - LMAX Disruptor BusySpinWaitStrategy (Thread.onSpinWait / pause).
 *  5. LMAX-Yielding          - LMAX Disruptor YieldingWaitStrategy (100 spins, then sched_yield).
 *  6. LMAX-Sleeping          - LMAX Disruptor SleepingWaitStrategy (spins, yields, then nanosleep).
 *  7. LMAX-Blocking          - LMAX Disruptor BlockingWaitStrategy (pthread_mutex + pthread_cond on all claims).
 *  8. LMAX-LiteBlocking      - LMAX Disruptor LiteBlockingWaitStrategy (atomic flag avoids mutex when active).
 *  9. LMAX-PhasedBackoff     - LMAX Disruptor PhasedBackoffWaitStrategy (spin -> yield -> LiteBlocking fallback).
 * 10. librbipc-Futex         - librbipc adaptive spin + sys_futex passive sleep.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include "bench_common.h"
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <assert.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <sched.h>

#include "rbipc.h"

/* ======================================================================
 * Configuration Constants
 * ====================================================================== */

#define NSEC_PER_SEC          1000000000ULL
#define RING_CAPACITY         4096U
#define CACHE_LINE_SIZE       64
#define MSG_PAYLOAD_SIZE      64U

#define TPUT_ITERATIONS       100000ULL
#define TPUT_WARMUP           1000ULL
#define LAT_ITERATIONS        10000ULL
#define WAKE_ITERATIONS       100ULL

#define COL_RESET   "\033[0m"
#define COL_BOLD    "\033[1m"
#define COL_RED     "\033[31m"
#define COL_GREEN   "\033[32m"
#define COL_YELLOW  "\033[33m"
#define COL_CYAN    "\033[36m"
#define COL_MAGENTA "\033[35m"

/* ======================================================================
 * Timing Utilities
 * ====================================================================== */

static inline uint64_t get_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * NSEC_PER_SEC + (uint64_t)ts.tv_nsec;
}

static inline uint64_t get_thread_cpu_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * NSEC_PER_SEC + (uint64_t)ts.tv_nsec;
}

static inline void cpu_pause(void) {
    __asm__ volatile("pause" ::: "memory");
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static uint64_t pct64(uint64_t *arr, size_t n, double p) {
    if (!n) return 0;
    size_t i = (size_t)(p / 100.0 * (double)n);
    if (i >= n) i = n - 1;
    return arr[i];
}

/* ======================================================================
 * Result Struct
 * ====================================================================== */

typedef struct {
    char     strategy_name[48];
    char     category[24];
    double   tput_mops;
    double   tput_mbs;
    uint64_t lat_min;
    uint64_t lat_p50;
    uint64_t lat_p90;
    uint64_t lat_p99;
    uint64_t lat_p999;
    uint64_t lat_max;
    uint64_t wake_min;
    uint64_t wake_p50;
    uint64_t wake_p90;
    uint64_t wake_p99;
    uint64_t wake_max;
    double   idle_cpu_pct;
    long     voluntary_ctx;
    long     involuntary_ctx;
} idle_bench_result_t;

/* ======================================================================
 * SYSTEM 1: DPDK rte_ring with Alternative Wait Strategies
 * ====================================================================== */

typedef struct {
    alignas(CACHE_LINE_SIZE) atomic_uint prod_head;
    alignas(CACHE_LINE_SIZE) atomic_uint prod_tail;
    alignas(CACHE_LINE_SIZE) atomic_uint cons_head;
    alignas(CACHE_LINE_SIZE) atomic_uint cons_tail;
    uint32_t mask;
    uint32_t capacity;

    int efd;
    int epfd;
    alignas(CACHE_LINE_SIZE) atomic_bool in_sleep;
} dpdk_ring_t;

static dpdk_ring_t *dpdk_ring_alloc(uint32_t cap) {
    assert((cap & (cap - 1)) == 0);
    size_t sz = sizeof(dpdk_ring_t) + cap * sizeof(uintptr_t);
    dpdk_ring_t *r = (dpdk_ring_t *)mmap(NULL, sz, PROT_READ | PROT_WRITE,
                                         MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (r == MAP_FAILED) return NULL;
    atomic_init(&r->prod_head, 0);
    atomic_init(&r->prod_tail, 0);
    atomic_init(&r->cons_head, 0);
    atomic_init(&r->cons_tail, 0);
    r->mask = cap - 1;
    r->capacity = cap;
    r->efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    r->epfd = epoll_create1(EPOLL_CLOEXEC);
    atomic_init(&r->in_sleep, false);

    if (r->epfd >= 0 && r->efd >= 0) {
        struct epoll_event ev = { .events = EPOLLIN, .data.fd = r->efd };
        epoll_ctl(r->epfd, EPOLL_CTL_ADD, r->efd, &ev);
    }
    return r;
}

static void dpdk_ring_reset(dpdk_ring_t *r) {
    if (!r) return;
    atomic_store_explicit(&r->prod_head, 0, memory_order_relaxed);
    atomic_store_explicit(&r->prod_tail, 0, memory_order_relaxed);
    atomic_store_explicit(&r->cons_head, 0, memory_order_relaxed);
    atomic_store_explicit(&r->cons_tail, 0, memory_order_relaxed);
    atomic_store_explicit(&r->in_sleep, false, memory_order_relaxed);
    if (r->efd >= 0) {
        uint64_t val;
        while (read(r->efd, &val, sizeof(val)) > 0) {}
    }
}

static void dpdk_ring_free(dpdk_ring_t *r) {
    if (!r) return;
    if (r->efd >= 0) close(r->efd);
    if (r->epfd >= 0) close(r->epfd);
    munmap(r, sizeof(dpdk_ring_t) + r->capacity * sizeof(uintptr_t));
}

static inline uintptr_t *dpdk_slots(dpdk_ring_t *r) {
    return (uintptr_t *)((char *)r + sizeof(dpdk_ring_t));
}

static int dpdk_try_enqueue(dpdk_ring_t *r, uintptr_t val) {
    uint32_t ph, pn, ct;
    for (;;) {
        ph = atomic_load_explicit(&r->prod_head, memory_order_relaxed);
        ct = atomic_load_explicit(&r->cons_tail, memory_order_acquire);
        pn = ph + 1;
        if (r->mask + ct - ph == 0) return -ENOBUFS;
        if (atomic_compare_exchange_weak_explicit(&r->prod_head, &ph, pn,
                memory_order_acquire, memory_order_relaxed)) break;
    }
    dpdk_slots(r)[ph & r->mask] = val;
    while (atomic_load_explicit(&r->prod_tail, memory_order_relaxed) != ph)
        cpu_pause();
    atomic_store_explicit(&r->prod_tail, pn, memory_order_release);

    if (atomic_load_explicit(&r->in_sleep, memory_order_seq_cst)) {
        uint64_t one = 1;
        (void)write(r->efd, &one, sizeof(one));
    }
    return 0;
}

static int dpdk_try_dequeue(dpdk_ring_t *r, uintptr_t *out) {
    uint32_t ch, cn, pt;
    for (;;) {
        ch = atomic_load_explicit(&r->cons_head, memory_order_relaxed);
        pt = atomic_load_explicit(&r->prod_tail, memory_order_acquire);
        if (ch == pt) return -ENOENT;
        cn = ch + 1;
        if (atomic_compare_exchange_weak_explicit(&r->cons_head, &ch, cn,
                memory_order_acquire, memory_order_relaxed)) break;
    }
    *out = dpdk_slots(r)[ch & r->mask];
    while (atomic_load_explicit(&r->cons_tail, memory_order_relaxed) != ch)
        cpu_pause();
    atomic_store_explicit(&r->cons_tail, cn, memory_order_release);
    return 0;
}

static void dpdk_dequeue_busyspin(dpdk_ring_t *r, uintptr_t *out) {
    while (dpdk_try_dequeue(r, out) == -ENOENT) {
        cpu_pause();
    }
}

static void dpdk_dequeue_pauseloop(dpdk_ring_t *r, uintptr_t *out) {
    uint32_t empty_polls = 0;
    while (dpdk_try_dequeue(r, out) == -ENOENT) {
        empty_polls++;
        if (empty_polls > 64) {
            for (int i = 0; i < 50; i++) cpu_pause();
        } else {
            cpu_pause();
        }
    }
}

static void dpdk_dequeue_interrupt(dpdk_ring_t *r, uintptr_t *out) {
    uint32_t empty_polls = 0;
    while (dpdk_try_dequeue(r, out) == -ENOENT) {
        empty_polls++;
        if (empty_polls > 64) {
            atomic_store_explicit(&r->in_sleep, true, memory_order_seq_cst);
            if (dpdk_try_dequeue(r, out) == 0) {
                atomic_store_explicit(&r->in_sleep, false, memory_order_seq_cst);
                return;
            }
            struct epoll_event ev;
            int nfds = epoll_wait(r->epfd, &ev, 1, 10);
            if (nfds > 0) {
                uint64_t val;
                (void)read(r->efd, &val, sizeof(val));
            }
            atomic_store_explicit(&r->in_sleep, false, memory_order_seq_cst);
            empty_polls = 0;
        } else {
            cpu_pause();
        }
    }
}

/* ======================================================================
 * SYSTEM 2: LMAX Disruptor with Distinct Wait Strategies
 * ====================================================================== */

typedef enum {
    LMAX_STRAT_BUSYSPIN,
    LMAX_STRAT_YIELDING,
    LMAX_STRAT_SLEEPING,
    LMAX_STRAT_BLOCKING,
    LMAX_STRAT_LITEBLOCKING,
    LMAX_STRAT_PHASEDBACKOFF
} lmax_strat_t;

typedef struct {
    alignas(CACHE_LINE_SIZE) atomic_llong prod_cursor;
    alignas(CACHE_LINE_SIZE) atomic_llong prod_claim;
    alignas(CACHE_LINE_SIZE) atomic_llong work_sequence;
    alignas(CACHE_LINE_SIZE) atomic_llong cons_gating;
    uint32_t capacity;
    uint32_t mask;

    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    alignas(CACHE_LINE_SIZE) atomic_bool signal_needed;
} lmax_ring_t;

static lmax_ring_t *lmax_ring_alloc(uint32_t cap) {
    assert((cap & (cap - 1)) == 0);
    size_t sz = sizeof(lmax_ring_t) + cap * sizeof(uintptr_t);
    lmax_ring_t *d = (lmax_ring_t *)mmap(NULL, sz, PROT_READ | PROT_WRITE,
                                         MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (d == MAP_FAILED) return NULL;
    atomic_init(&d->prod_cursor, -1);
    atomic_init(&d->prod_claim, -1);
    atomic_init(&d->work_sequence, -1);
    atomic_init(&d->cons_gating, -1);
    d->capacity = cap;
    d->mask = cap - 1;

    pthread_mutexattr_t mattr;
    pthread_mutexattr_init(&mattr);
    pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&d->mutex, &mattr);
    pthread_mutexattr_destroy(&mattr);

    pthread_condattr_t cattr;
    pthread_condattr_init(&cattr);
    pthread_condattr_setpshared(&cattr, PTHREAD_PROCESS_SHARED);
    pthread_cond_init(&d->cond, &cattr);
    pthread_condattr_destroy(&cattr);

    atomic_init(&d->signal_needed, false);
    return d;
}

static void lmax_ring_reset(lmax_ring_t *d) {
    if (!d) return;
    atomic_store_explicit(&d->prod_cursor, -1, memory_order_relaxed);
    atomic_store_explicit(&d->prod_claim, -1, memory_order_relaxed);
    atomic_store_explicit(&d->work_sequence, -1, memory_order_relaxed);
    atomic_store_explicit(&d->cons_gating, -1, memory_order_relaxed);
    atomic_store_explicit(&d->signal_needed, false, memory_order_relaxed);
}

static void lmax_ring_free(lmax_ring_t *d) {
    if (!d) return;
    pthread_mutex_destroy(&d->mutex);
    pthread_cond_destroy(&d->cond);
    munmap(d, sizeof(lmax_ring_t) + d->capacity * sizeof(uintptr_t));
}

static inline uintptr_t *lmax_slots(lmax_ring_t *d) {
    return (uintptr_t *)((char *)d + sizeof(lmax_ring_t));
}

static inline void lmax_publish(lmax_ring_t *d, int64_t seq, uintptr_t val, lmax_strat_t strat) {
    while (seq - atomic_load_explicit(&d->cons_gating, memory_order_acquire) >= (int64_t)d->capacity) {
        cpu_pause();
    }
    lmax_slots(d)[(uint32_t)seq & d->mask] = val;
    atomic_store_explicit(&d->prod_cursor, seq, memory_order_release);

    if (strat == LMAX_STRAT_BLOCKING) {
        pthread_mutex_lock(&d->mutex);
        pthread_cond_broadcast(&d->cond);
        pthread_mutex_unlock(&d->mutex);
    } else if (strat == LMAX_STRAT_LITEBLOCKING || strat == LMAX_STRAT_PHASEDBACKOFF) {
        if (atomic_exchange_explicit(&d->signal_needed, false, memory_order_acq_rel)) {
            pthread_mutex_lock(&d->mutex);
            pthread_cond_broadcast(&d->cond);
            pthread_mutex_unlock(&d->mutex);
        }
    }
}

static inline bool lmax_consume(lmax_ring_t *d, int64_t max_seq, uintptr_t *out, lmax_strat_t strat) {
    int64_t cur, next;
    do {
        cur = atomic_load_explicit(&d->work_sequence, memory_order_relaxed);
        next = cur + 1;
        if (max_seq >= 0 && next >= max_seq) return false;
    } while (!atomic_compare_exchange_weak_explicit(&d->work_sequence, &cur, next,
                                                    memory_order_acq_rel, memory_order_relaxed));

    switch (strat) {
        case LMAX_STRAT_BUSYSPIN:
            while (atomic_load_explicit(&d->prod_cursor, memory_order_acquire) < next) {
                cpu_pause();
            }
            break;

        case LMAX_STRAT_YIELDING: {
            int counter = 100;
            while (atomic_load_explicit(&d->prod_cursor, memory_order_acquire) < next) {
                if (counter == 0) {
                    sched_yield();
                } else {
                    counter--;
                    cpu_pause();
                }
            }
            break;
        }

        case LMAX_STRAT_SLEEPING: {
            int counter = 200;
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000 };
            while (atomic_load_explicit(&d->prod_cursor, memory_order_acquire) < next) {
                if (counter > 100) {
                    counter--;
                    cpu_pause();
                } else if (counter > 0) {
                    counter--;
                    sched_yield();
                } else {
                    nanosleep(&ts, NULL);
                }
            }
            break;
        }

        case LMAX_STRAT_BLOCKING: {
            if (atomic_load_explicit(&d->prod_cursor, memory_order_acquire) < next) {
                pthread_mutex_lock(&d->mutex);
                while (atomic_load_explicit(&d->prod_cursor, memory_order_acquire) < next) {
                    pthread_cond_wait(&d->cond, &d->mutex);
                }
                pthread_mutex_unlock(&d->mutex);
            }
            break;
        }

        case LMAX_STRAT_LITEBLOCKING: {
            if (atomic_load_explicit(&d->prod_cursor, memory_order_acquire) < next) {
                pthread_mutex_lock(&d->mutex);
                do {
                    atomic_store_explicit(&d->signal_needed, true, memory_order_release);
                    if (atomic_load_explicit(&d->prod_cursor, memory_order_acquire) >= next)
                        break;
                    pthread_cond_wait(&d->cond, &d->mutex);
                } while (atomic_load_explicit(&d->prod_cursor, memory_order_acquire) < next);
                pthread_mutex_unlock(&d->mutex);
            }
            break;
        }

        case LMAX_STRAT_PHASEDBACKOFF: {
            if (atomic_load_explicit(&d->prod_cursor, memory_order_acquire) >= next) break;
            uint64_t t0 = get_ns();
            bool done = false;
            while (!done) {
                if (atomic_load_explicit(&d->prod_cursor, memory_order_acquire) >= next) {
                    done = true;
                    break;
                }
                uint64_t delta = get_ns() - t0;
                if (delta < 1000) {
                    cpu_pause();
                } else if (delta < 10000) {
                    sched_yield();
                } else {
                    pthread_mutex_lock(&d->mutex);
                    do {
                        atomic_store_explicit(&d->signal_needed, true, memory_order_release);
                        if (atomic_load_explicit(&d->prod_cursor, memory_order_acquire) >= next)
                            break;
                        pthread_cond_wait(&d->cond, &d->mutex);
                    } while (atomic_load_explicit(&d->prod_cursor, memory_order_acquire) < next);
                    pthread_mutex_unlock(&d->mutex);
                    done = true;
                }
            }
            break;
        }
    }

    *out = lmax_slots(d)[(uint32_t)next & d->mask];
    atomic_store_explicit(&d->cons_gating, next, memory_order_release);
    return true;
}

/* ======================================================================
 * SYSTEM 3: librbipc (Futex + Adaptive Spin)
 * ====================================================================== */

typedef struct {
    rbipc_ring_t *ring_ab_prod;
    rbipc_ring_t *ring_ab_cons;
    rbipc_ring_t *ring_ba_prod;
    rbipc_ring_t *ring_ba_cons;
} rbipc_env_t;

static rbipc_env_t rbipc_init(const char *name_ab, const char *name_ba) {
    rbipc_destroy(name_ab);
    rbipc_destroy(name_ba);
    rbipc_env_t env;
    memset(&env, 0, sizeof(env));
    int rc1 = rbipc_create(name_ab, RING_CAPACITY, 128, &env.ring_ab_prod);
    int rc2 = rbipc_create(name_ba, RING_CAPACITY, 128, &env.ring_ba_prod);
    assert(rc1 == RBIPC_OK && rc2 == RBIPC_OK);
    int rc3 = rbipc_attach(name_ab, &env.ring_ab_cons);
    int rc4 = rbipc_attach(name_ba, &env.ring_ba_cons);
    assert(rc3 == RBIPC_OK && rc4 == RBIPC_OK);
    return env;
}

static void rbipc_cleanup(rbipc_env_t *env, const char *name_ab, const char *name_ba) {
    if (env->ring_ab_prod) rbipc_detach(env->ring_ab_prod);
    if (env->ring_ab_cons) rbipc_detach(env->ring_ab_cons);
    if (env->ring_ba_prod) rbipc_detach(env->ring_ba_prod);
    if (env->ring_ba_cons) rbipc_detach(env->ring_ba_cons);
    rbipc_destroy(name_ab);
    rbipc_destroy(name_ba);
}

/* ======================================================================
 * BENCHMARK RUNNERS
 * ====================================================================== */

typedef struct {
    dpdk_ring_t *r;
    int strat;
    atomic_bool *start;
    atomic_bool *done;
} dpdk_th_arg_t;

static void *dpdk_tput_worker(void *p) {
    dpdk_th_arg_t *a = (dpdk_th_arg_t *)p;
    while (!atomic_load_explicit(a->start, memory_order_acquire)) cpu_pause();
    uintptr_t val;
    for (uint64_t i = 0; i < TPUT_ITERATIONS; i++) {
        if (a->strat == 0) dpdk_dequeue_busyspin(a->r, &val);
        else if (a->strat == 1) dpdk_dequeue_pauseloop(a->r, &val);
        else dpdk_dequeue_interrupt(a->r, &val);
    }
    atomic_store_explicit(a->done, true, memory_order_release);
    return NULL;
}

static double run_dpdk_tput(dpdk_ring_t *r, int strat_mode) {
    atomic_bool start = false, done = false;
    dpdk_th_arg_t arg = { r, strat_mode, &start, &done };

    pthread_t th;
    pthread_create(&th, NULL, dpdk_tput_worker, &arg);

    for (uint64_t i = 0; i < TPUT_WARMUP; i++) {
        while (dpdk_try_enqueue(r, (uintptr_t)i) == -ENOBUFS) cpu_pause();
        uintptr_t tmp;
        while (dpdk_try_dequeue(r, &tmp) == -ENOENT) cpu_pause();
    }

    uint64_t t0 = get_ns();
    atomic_store_explicit(&start, true, memory_order_release);
    for (uint64_t i = 0; i < TPUT_ITERATIONS; i++) {
        while (dpdk_try_enqueue(r, (uintptr_t)i) == -ENOBUFS) cpu_pause();
    }
    pthread_join(th, NULL);
    uint64_t t1 = get_ns();

    double sec = (double)(t1 - t0) / 1e9;
    return (double)TPUT_ITERATIONS / sec;
}

typedef struct {
    lmax_ring_t *d;
    lmax_strat_t strat;
    atomic_bool *start;
    atomic_bool *done;
} lmax_th_arg_t;

static void *lmax_tput_worker(void *p) {
    lmax_th_arg_t *a = (lmax_th_arg_t *)p;
    while (!atomic_load_explicit(a->start, memory_order_acquire)) cpu_pause();
    uintptr_t val;
    for (uint64_t i = 0; i < TPUT_ITERATIONS; i++) {
        lmax_consume(a->d, (int64_t)TPUT_ITERATIONS, &val, a->strat);
    }
    atomic_store_explicit(a->done, true, memory_order_release);
    return NULL;
}

static double run_lmax_tput(lmax_ring_t *d, lmax_strat_t strat) {
    atomic_bool start = false, done = false;
    lmax_th_arg_t arg = { d, strat, &start, &done };

    pthread_t th;
    pthread_create(&th, NULL, lmax_tput_worker, &arg);

    uint64_t t0 = get_ns();
    atomic_store_explicit(&start, true, memory_order_release);
    for (uint64_t i = 0; i < TPUT_ITERATIONS; i++) {
        lmax_publish(d, (int64_t)i, (uintptr_t)i, strat);
    }
    pthread_join(th, NULL);
    uint64_t t1 = get_ns();

    double sec = (double)(t1 - t0) / 1e9;
    return (double)TPUT_ITERATIONS / sec;
}

typedef struct {
    rbipc_ring_t *r;
    atomic_bool *start;
    atomic_bool *done;
} rbipc_th_arg_t;

static void *rbipc_tput_worker(void *p) {
    rbipc_th_arg_t *a = (rbipc_th_arg_t *)p;
    while (!atomic_load_explicit(a->start, memory_order_acquire)) cpu_pause();
    const void *buf;
    uint32_t len;
    uint64_t ticket;
    for (uint64_t i = 0; i < TPUT_ITERATIONS; i++) {
        while (rbipc_read_acquire(a->r, &buf, &len, &ticket) != RBIPC_OK) cpu_pause();
        rbipc_read_release(a->r, ticket);
    }
    atomic_store_explicit(a->done, true, memory_order_release);
    return NULL;
}

static double run_rbipc_tput(rbipc_env_t *env) {
    atomic_bool start = false, done = false;
    rbipc_th_arg_t arg = { env->ring_ab_cons, &start, &done };

    pthread_t th;
    pthread_create(&th, NULL, rbipc_tput_worker, &arg);

    uint64_t t0 = get_ns();
    atomic_store_explicit(&start, true, memory_order_release);
    void *wbuf;
    uint64_t wtick;
    for (uint64_t i = 0; i < TPUT_ITERATIONS; i++) {
        while (rbipc_reserve_write(env->ring_ab_prod, MSG_PAYLOAD_SIZE, &wbuf, &wtick) != RBIPC_OK) cpu_pause();
        rbipc_commit_write(env->ring_ab_prod, wtick, MSG_PAYLOAD_SIZE);
    }
    pthread_join(th, NULL);
    uint64_t t1 = get_ns();

    double sec = (double)(t1 - t0) / 1e9;
    return (double)TPUT_ITERATIONS / sec;
}

/* --- Latency Helpers --- */

typedef struct {
    dpdk_ring_t *ab;
    dpdk_ring_t *ba;
    int strat;
    atomic_bool *stop;
} dpdk_lat_arg_t;

static void *dpdk_lat_worker(void *p) {
    dpdk_lat_arg_t *a = (dpdk_lat_arg_t *)p;
    uintptr_t val;
    while (!atomic_load_explicit(a->stop, memory_order_acquire)) {
        if (a->strat == 0) dpdk_dequeue_busyspin(a->ab, &val);
        else if (a->strat == 1) dpdk_dequeue_pauseloop(a->ab, &val);
        else dpdk_dequeue_interrupt(a->ab, &val);
        if (val == UINTPTR_MAX) break;
        while (dpdk_try_enqueue(a->ba, val) == -ENOBUFS) cpu_pause();
    }
    return NULL;
}

static void run_dpdk_lat(dpdk_ring_t *ab, dpdk_ring_t *ba, int strat, uint64_t *samples) {
    atomic_bool stop = false;
    dpdk_lat_arg_t arg = { ab, ba, strat, &stop };

    pthread_t th;
    pthread_create(&th, NULL, dpdk_lat_worker, &arg);

    uintptr_t resp;
    for (uint64_t i = 0; i < LAT_ITERATIONS; i++) {
        uint64_t t0 = get_ns();
        while (dpdk_try_enqueue(ab, (uintptr_t)i) == -ENOBUFS) cpu_pause();
        if (strat == 0) dpdk_dequeue_busyspin(ba, &resp);
        else if (strat == 1) dpdk_dequeue_pauseloop(ba, &resp);
        else dpdk_dequeue_interrupt(ba, &resp);
        uint64_t t1 = get_ns();
        samples[i] = (t1 - t0) / 2;
    }
    atomic_store_explicit(&stop, true, memory_order_release);
    while (dpdk_try_enqueue(ab, UINTPTR_MAX) == -ENOBUFS) cpu_pause();
    pthread_join(th, NULL);
}

typedef struct {
    lmax_ring_t *ab;
    lmax_ring_t *ba;
    lmax_strat_t strat;
    atomic_bool *stop;
} lmax_lat_arg_t;

static void *lmax_lat_worker(void *p) {
    lmax_lat_arg_t *a = (lmax_lat_arg_t *)p;
    uintptr_t val;
    int64_t seq = 0;
    while (!atomic_load_explicit(a->stop, memory_order_acquire)) {
        lmax_consume(a->ab, -1, &val, a->strat);
        if (val == UINTPTR_MAX) break;
        lmax_publish(a->ba, seq++, val, a->strat);
    }
    return NULL;
}

static void run_lmax_lat(lmax_ring_t *ab, lmax_ring_t *ba, lmax_strat_t strat, uint64_t *samples) {
    atomic_bool stop = false;
    lmax_lat_arg_t arg = { ab, ba, strat, &stop };

    pthread_t th;
    pthread_create(&th, NULL, lmax_lat_worker, &arg);

    uintptr_t resp;
    for (uint64_t i = 0; i < LAT_ITERATIONS; i++) {
        uint64_t t0 = get_ns();
        lmax_publish(ab, (int64_t)i, (uintptr_t)i, strat);
        lmax_consume(ba, -1, &resp, strat);
        uint64_t t1 = get_ns();
        samples[i] = (t1 - t0) / 2;
    }
    atomic_store_explicit(&stop, true, memory_order_release);
    lmax_publish(ab, (int64_t)LAT_ITERATIONS, UINTPTR_MAX, strat);
    pthread_join(th, NULL);
}

typedef struct {
    rbipc_ring_t *ab;
    rbipc_ring_t *ba;
    atomic_bool *stop;
} rbipc_lat_arg_t;

static void *rbipc_lat_worker(void *p) {
    rbipc_lat_arg_t *a = (rbipc_lat_arg_t *)p;
    const void *rbuf;
    uint32_t rlen;
    uint64_t rtick;
    void *wbuf;
    uint64_t wtick;
    while (!atomic_load_explicit(a->stop, memory_order_acquire)) {
        if (rbipc_read_acquire(a->ab, &rbuf, &rlen, &rtick) == RBIPC_OK) {
            if (rlen == 0) { rbipc_read_release(a->ab, rtick); break; }
            while (rbipc_reserve_write(a->ba, rlen, &wbuf, &wtick) != RBIPC_OK) cpu_pause();
            rbipc_commit_write(a->ba, wtick, rlen);
            rbipc_read_release(a->ab, rtick);
        }
    }
    return NULL;
}

static void run_rbipc_lat(rbipc_env_t *env, uint64_t *samples) {
    atomic_bool stop = false;
    rbipc_lat_arg_t arg = { env->ring_ab_cons, env->ring_ba_prod, &stop };

    pthread_t th;
    pthread_create(&th, NULL, rbipc_lat_worker, &arg);

    const void *rbuf;
    uint32_t rlen;
    uint64_t rtick;
    void *wbuf;
    uint64_t wtick;

    for (uint64_t i = 0; i < LAT_ITERATIONS; i++) {
        uint64_t t0 = get_ns();
        while (rbipc_reserve_write(env->ring_ab_prod, MSG_PAYLOAD_SIZE, &wbuf, &wtick) != RBIPC_OK) cpu_pause();
        rbipc_commit_write(env->ring_ab_prod, wtick, MSG_PAYLOAD_SIZE);
        while (rbipc_read_acquire(env->ring_ba_cons, &rbuf, &rlen, &rtick) != RBIPC_OK) cpu_pause();
        rbipc_read_release(env->ring_ba_cons, rtick);
        uint64_t t1 = get_ns();
        samples[i] = (t1 - t0) / 2;
    }
    atomic_store_explicit(&stop, true, memory_order_release);
    while (rbipc_reserve_write(env->ring_ab_prod, 0, &wbuf, &wtick) != RBIPC_OK) cpu_pause();
    rbipc_commit_write(env->ring_ab_prod, wtick, 0);
    pthread_join(th, NULL);
}

/* --- Quiescent Wake Helpers --- */

typedef struct {
    dpdk_ring_t *r;
    int strat;
    atomic_bool *stop;
    _Atomic uint64_t *recv_ts;
} dpdk_wake_arg_t;

static void *dpdk_wake_worker(void *p) {
    dpdk_wake_arg_t *a = (dpdk_wake_arg_t *)p;
    uintptr_t val;
    while (!atomic_load_explicit(a->stop, memory_order_acquire)) {
        if (a->strat == 0) dpdk_dequeue_busyspin(a->r, &val);
        else if (a->strat == 1) dpdk_dequeue_pauseloop(a->r, &val);
        else dpdk_dequeue_interrupt(a->r, &val);
        uint64_t now = get_ns();
        if (val == UINTPTR_MAX) break;
        atomic_store_explicit(a->recv_ts, now, memory_order_release);
    }
    return NULL;
}

static void run_dpdk_wake(dpdk_ring_t *r, int strat, uint64_t *wake_samples) {
    atomic_bool stop = false;
    alignas(CACHE_LINE_SIZE) _Atomic uint64_t recv_ts = 0;
    dpdk_wake_arg_t arg = { r, strat, &stop, &recv_ts };

    pthread_t th;
    pthread_create(&th, NULL, dpdk_wake_worker, &arg);

    struct timespec sleep_gap = { .tv_sec = 0, .tv_nsec = 15000000 };
    for (uint64_t i = 0; i < WAKE_ITERATIONS; i++) {
        nanosleep(&sleep_gap, NULL);
        atomic_store_explicit(&recv_ts, 0, memory_order_relaxed);
        uint64_t send_ts = get_ns();
        while (dpdk_try_enqueue(r, (uintptr_t)i) == -ENOBUFS) cpu_pause();
        while (atomic_load_explicit(&recv_ts, memory_order_acquire) == 0) cpu_pause();
        uint64_t rts = atomic_load_explicit(&recv_ts, memory_order_acquire);
        wake_samples[i] = (rts > send_ts) ? (rts - send_ts) : 0;
    }

    atomic_store_explicit(&stop, true, memory_order_release);
    while (dpdk_try_enqueue(r, UINTPTR_MAX) == -ENOBUFS) cpu_pause();
    pthread_join(th, NULL);
}

typedef struct {
    lmax_ring_t *d;
    lmax_strat_t strat;
    atomic_bool *stop;
    _Atomic uint64_t *recv_ts;
} lmax_wake_arg_t;

static void *lmax_wake_worker(void *p) {
    lmax_wake_arg_t *a = (lmax_wake_arg_t *)p;
    uintptr_t val;
    while (!atomic_load_explicit(a->stop, memory_order_acquire)) {
        lmax_consume(a->d, -1, &val, a->strat);
        uint64_t now = get_ns();
        if (val == UINTPTR_MAX) break;
        atomic_store_explicit(a->recv_ts, now, memory_order_release);
    }
    return NULL;
}

static void run_lmax_wake(lmax_ring_t *d, lmax_strat_t strat, uint64_t *wake_samples) {
    atomic_bool stop = false;
    alignas(CACHE_LINE_SIZE) _Atomic uint64_t recv_ts = 0;
    lmax_wake_arg_t arg = { d, strat, &stop, &recv_ts };

    pthread_t th;
    pthread_create(&th, NULL, lmax_wake_worker, &arg);

    struct timespec sleep_gap = { .tv_sec = 0, .tv_nsec = 15000000 };
    for (uint64_t i = 0; i < WAKE_ITERATIONS; i++) {
        nanosleep(&sleep_gap, NULL);
        atomic_store_explicit(&recv_ts, 0, memory_order_relaxed);
        uint64_t send_ts = get_ns();
        lmax_publish(d, (int64_t)i, (uintptr_t)i, strat);
        while (atomic_load_explicit(&recv_ts, memory_order_acquire) == 0) cpu_pause();
        uint64_t rts = atomic_load_explicit(&recv_ts, memory_order_acquire);
        wake_samples[i] = (rts > send_ts) ? (rts - send_ts) : 0;
    }

    atomic_store_explicit(&stop, true, memory_order_release);
    lmax_publish(d, (int64_t)WAKE_ITERATIONS, UINTPTR_MAX, strat);
    pthread_join(th, NULL);
}

typedef struct {
    rbipc_ring_t *r;
    atomic_bool *stop;
    _Atomic uint64_t *recv_ts;
} rbipc_wake_arg_t;

static void *rbipc_wake_worker(void *p) {
    rbipc_wake_arg_t *a = (rbipc_wake_arg_t *)p;
    const void *buf;
    uint32_t len;
    uint64_t ticket;
    while (!atomic_load_explicit(a->stop, memory_order_acquire)) {
        if (rbipc_read_acquire(a->r, &buf, &len, &ticket) == RBIPC_OK) {
            uint64_t now = get_ns();
            if (len == 0) { rbipc_read_release(a->r, ticket); break; }
            atomic_store_explicit(a->recv_ts, now, memory_order_release);
            rbipc_read_release(a->r, ticket);
        }
    }
    return NULL;
}

static void run_rbipc_wake(rbipc_env_t *env, uint64_t *wake_samples) {
    atomic_bool stop = false;
    alignas(CACHE_LINE_SIZE) _Atomic uint64_t recv_ts = 0;
    rbipc_wake_arg_t arg = { env->ring_ab_cons, &stop, &recv_ts };

    pthread_t th;
    pthread_create(&th, NULL, rbipc_wake_worker, &arg);

    void *wbuf;
    uint64_t wtick;
    struct timespec sleep_gap = { .tv_sec = 0, .tv_nsec = 15000000 };
    for (uint64_t i = 0; i < WAKE_ITERATIONS; i++) {
        nanosleep(&sleep_gap, NULL);
        atomic_store_explicit(&recv_ts, 0, memory_order_relaxed);
        uint64_t send_ts = get_ns();
        while (rbipc_reserve_write(env->ring_ab_prod, MSG_PAYLOAD_SIZE, &wbuf, &wtick) != RBIPC_OK) cpu_pause();
        rbipc_commit_write(env->ring_ab_prod, wtick, MSG_PAYLOAD_SIZE);
        while (atomic_load_explicit(&recv_ts, memory_order_acquire) == 0) cpu_pause();
        uint64_t rts = atomic_load_explicit(&recv_ts, memory_order_acquire);
        wake_samples[i] = (rts > send_ts) ? (rts - send_ts) : 0;
    }

    atomic_store_explicit(&stop, true, memory_order_release);
    while (rbipc_reserve_write(env->ring_ab_prod, 0, &wbuf, &wtick) != RBIPC_OK) cpu_pause();
    rbipc_commit_write(env->ring_ab_prod, wtick, 0);
    pthread_join(th, NULL);
}

/* --- Idle CPU Helpers --- */

typedef struct {
    dpdk_ring_t *r;
    int strat;
    atomic_bool *start;
    atomic_bool *stop;
    _Atomic uint64_t *cpu_time_spent;
    long *vol_ctx;
    long *invol_ctx;
} dpdk_idle_arg_t;

static void *dpdk_idle_worker(void *p) {
    dpdk_idle_arg_t *a = (dpdk_idle_arg_t *)p;
    while (!atomic_load_explicit(a->start, memory_order_acquire)) cpu_pause();
    uint64_t cpu0 = get_thread_cpu_ns();
    struct rusage ru0;
    getrusage(RUSAGE_THREAD, &ru0);

    while (!atomic_load_explicit(a->stop, memory_order_acquire)) {
        if (a->strat == 0) {
            cpu_pause();
        } else if (a->strat == 1) {
            for (int i = 0; i < 50; i++) cpu_pause();
        } else {
            /* Interrupt mode wait with 10ms timeout */
            struct epoll_event ev;
            (void)epoll_wait(a->r->epfd, &ev, 1, 10);
        }
    }
    uint64_t cpu1 = get_thread_cpu_ns();
    struct rusage ru1;
    getrusage(RUSAGE_THREAD, &ru1);
    atomic_store_explicit(a->cpu_time_spent, (cpu1 - cpu0), memory_order_release);
    *(a->vol_ctx) = ru1.ru_nvcsw - ru0.ru_nvcsw;
    *(a->invol_ctx) = ru1.ru_nivcsw - ru0.ru_nivcsw;
    return NULL;
}

static double measure_dpdk_idle_cpu(dpdk_ring_t *r, int strat, long *vol_ctx, long *invol_ctx) {
    atomic_bool start = false, stop = false;
    alignas(CACHE_LINE_SIZE) _Atomic uint64_t cpu_time_spent = 0;
    dpdk_idle_arg_t arg = { r, strat, &start, &stop, &cpu_time_spent, vol_ctx, invol_ctx };

    pthread_t th;
    pthread_create(&th, NULL, dpdk_idle_worker, &arg);

    uint64_t w0 = get_ns();
    atomic_store_explicit(&start, true, memory_order_release);
    struct timespec sleep_sec = { .tv_sec = 1, .tv_nsec = 0 };
    nanosleep(&sleep_sec, NULL);
    atomic_store_explicit(&stop, true, memory_order_release);

    pthread_join(th, NULL);
    uint64_t w1 = get_ns();

    double wall_ns = (double)(w1 - w0);
    double cpu_ns  = (double)atomic_load_explicit(&cpu_time_spent, memory_order_acquire);
    return (cpu_ns / wall_ns) * 100.0;
}

typedef struct {
    lmax_ring_t *d;
    lmax_strat_t strat;
    atomic_bool *start;
    atomic_bool *stop;
    _Atomic uint64_t *cpu_time_spent;
    long *vol_ctx;
    long *invol_ctx;
} lmax_idle_arg_t;

static void *lmax_idle_worker(void *p) {
    lmax_idle_arg_t *a = (lmax_idle_arg_t *)p;
    while (!atomic_load_explicit(a->start, memory_order_acquire)) cpu_pause();
    uint64_t cpu0 = get_thread_cpu_ns();
    struct rusage ru0;
    getrusage(RUSAGE_THREAD, &ru0);

    while (!atomic_load_explicit(a->stop, memory_order_acquire)) {
        if (a->strat == LMAX_STRAT_BUSYSPIN) {
            cpu_pause();
        } else if (a->strat == LMAX_STRAT_YIELDING) {
            sched_yield();
        } else if (a->strat == LMAX_STRAT_SLEEPING) {
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000 };
            nanosleep(&ts, NULL);
        } else {
            pthread_mutex_lock(&a->d->mutex);
            atomic_store_explicit(&a->d->signal_needed, true, memory_order_release);
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 10000000;
            if (ts.tv_nsec >= 1000000000) {
                ts.tv_sec += 1;
                ts.tv_nsec -= 1000000000;
            }
            pthread_cond_timedwait(&a->d->cond, &a->d->mutex, &ts);
            pthread_mutex_unlock(&a->d->mutex);
        }
    }
    uint64_t cpu1 = get_thread_cpu_ns();
    struct rusage ru1;
    getrusage(RUSAGE_THREAD, &ru1);
    atomic_store_explicit(a->cpu_time_spent, (cpu1 - cpu0), memory_order_release);
    *(a->vol_ctx) = ru1.ru_nvcsw - ru0.ru_nvcsw;
    *(a->invol_ctx) = ru1.ru_nivcsw - ru0.ru_nivcsw;
    return NULL;
}

static double measure_lmax_idle_cpu(lmax_ring_t *d, lmax_strat_t strat, long *vol_ctx, long *invol_ctx) {
    atomic_bool start = false, stop = false;
    alignas(CACHE_LINE_SIZE) _Atomic uint64_t cpu_time_spent = 0;
    lmax_idle_arg_t arg = { d, strat, &start, &stop, &cpu_time_spent, vol_ctx, invol_ctx };

    pthread_t th;
    pthread_create(&th, NULL, lmax_idle_worker, &arg);

    uint64_t w0 = get_ns();
    atomic_store_explicit(&start, true, memory_order_release);
    struct timespec sleep_sec = { .tv_sec = 1, .tv_nsec = 0 };
    nanosleep(&sleep_sec, NULL);
    atomic_store_explicit(&stop, true, memory_order_release);

    pthread_join(th, NULL);
    uint64_t w1 = get_ns();

    double wall_ns = (double)(w1 - w0);
    double cpu_ns  = (double)atomic_load_explicit(&cpu_time_spent, memory_order_acquire);
    return (cpu_ns / wall_ns) * 100.0;
}

typedef struct {
    rbipc_ring_t *r;
    atomic_bool *start;
    atomic_bool *stop;
    _Atomic uint64_t *cpu_time_spent;
    long *vol_ctx;
    long *invol_ctx;
} rbipc_idle_arg_t;

static void *rbipc_idle_worker(void *p) {
    rbipc_idle_arg_t *a = (rbipc_idle_arg_t *)p;
    const void *buf;
    uint32_t len;
    uint64_t ticket;
    while (!atomic_load_explicit(a->start, memory_order_acquire)) cpu_pause();
    uint64_t cpu0 = get_thread_cpu_ns();
    struct rusage ru0;
    getrusage(RUSAGE_THREAD, &ru0);

    while (!atomic_load_explicit(a->stop, memory_order_acquire)) {
        int rc = rbipc_read_acquire_timeout(a->r, 50000000ULL, &buf, &len, &ticket);
        if (rc == RBIPC_OK) {
            rbipc_read_release(a->r, ticket);
            break;
        }
    }
    uint64_t cpu1 = get_thread_cpu_ns();
    struct rusage ru1;
    getrusage(RUSAGE_THREAD, &ru1);
    atomic_store_explicit(a->cpu_time_spent, (cpu1 - cpu0), memory_order_release);
    *(a->vol_ctx) = ru1.ru_nvcsw - ru0.ru_nvcsw;
    *(a->invol_ctx) = ru1.ru_nivcsw - ru0.ru_nivcsw;
    return NULL;
}

static double measure_rbipc_idle_cpu(rbipc_env_t *env, long *vol_ctx, long *invol_ctx) {
    atomic_bool start = false, stop = false;
    alignas(CACHE_LINE_SIZE) _Atomic uint64_t cpu_time_spent = 0;
    rbipc_idle_arg_t arg = { env->ring_ab_cons, &start, &stop, &cpu_time_spent, vol_ctx, invol_ctx };

    pthread_t th;
    pthread_create(&th, NULL, rbipc_idle_worker, &arg);

    uint64_t w0 = get_ns();
    atomic_store_explicit(&start, true, memory_order_release);
    struct timespec sleep_sec = { .tv_sec = 1, .tv_nsec = 0 };
    nanosleep(&sleep_sec, NULL);
    atomic_store_explicit(&stop, true, memory_order_release);

    pthread_join(th, NULL);
    uint64_t w1 = get_ns();

    double wall_ns = (double)(w1 - w0);
    double cpu_ns  = (double)atomic_load_explicit(&cpu_time_spent, memory_order_acquire);
    return (cpu_ns / wall_ns) * 100.0;
}

/* ======================================================================
 * Main Test Orchestrator
 * ====================================================================== */

static void record_res(idle_bench_result_t *results, size_t *n_res,
                       const char *name, const char *cat, double tput,
                       uint64_t *lats, uint64_t *wakes, double idle_cpu, long vol, long invol) {
    qsort(lats, LAT_ITERATIONS, sizeof(uint64_t), cmp_u64);
    qsort(wakes, WAKE_ITERATIONS, sizeof(uint64_t), cmp_u64);
    idle_bench_result_t *r = &results[(*n_res)++];
    strncpy(r->strategy_name, name, sizeof(r->strategy_name) - 1);
    strncpy(r->category, cat, sizeof(r->category) - 1);
    r->tput_mops = tput / 1e6;
    r->tput_mbs  = (tput * (double)MSG_PAYLOAD_SIZE) / (1024.0 * 1024.0);
    r->lat_min   = lats[0];
    r->lat_p50   = pct64(lats, LAT_ITERATIONS, 50.0);
    r->lat_p90   = pct64(lats, LAT_ITERATIONS, 90.0);
    r->lat_p99   = pct64(lats, LAT_ITERATIONS, 99.0);
    r->lat_p999  = pct64(lats, LAT_ITERATIONS, 99.9);
    r->lat_max   = lats[LAT_ITERATIONS - 1];
    r->wake_min  = wakes[0];
    r->wake_p50  = pct64(wakes, WAKE_ITERATIONS, 50.0);
    r->wake_p90  = pct64(wakes, WAKE_ITERATIONS, 90.0);
    r->wake_p99  = pct64(wakes, WAKE_ITERATIONS, 99.0);
    r->wake_max  = wakes[WAKE_ITERATIONS - 1];
    r->idle_cpu_pct = idle_cpu;
    r->voluntary_ctx = vol;
    r->involuntary_ctx = invol;

    printf("%s[%-22s]%s Tput: %s%6.2f Mops/s%s (%6.1f MB/s) | Lat p50: %s%5lu ns%s | Wake p50: %s%5lu ns%s | Idle CPU: %s%5.1f%%%s\n",
           COL_BOLD, name, COL_RESET,
           COL_GREEN, r->tput_mops, COL_RESET, r->tput_mbs,
           COL_YELLOW, r->lat_p50, COL_RESET,
           COL_MAGENTA, r->wake_p50, COL_RESET,
           (idle_cpu > 50.0) ? COL_RED : COL_GREEN, idle_cpu, COL_RESET);
    fflush(stdout);
}

int main(void) {
    printf("%s%s========================================================================================%s\n",
           COL_BOLD, COL_CYAN, COL_RESET);
    printf("%s%s   EXHAUSTIVE INVESTIGATION: DPDK & LMAX DISRUPTOR IDLE & WAIT STRATEGIES BENCHMARK   %s\n",
           COL_BOLD, COL_YELLOW, COL_RESET);
    printf("%s%s========================================================================================%s\n\n",
           COL_BOLD, COL_CYAN, COL_RESET);
    fflush(stdout);

    uint64_t *lat_buf  = (uint64_t *)malloc(LAT_ITERATIONS * sizeof(uint64_t));
    uint64_t *wake_buf = (uint64_t *)malloc(WAKE_ITERATIONS * sizeof(uint64_t));
    assert(lat_buf && wake_buf);

    idle_bench_result_t results[10];
    size_t n_res = 0;

    printf("%s--- Benchmarking DPDK rte_ring Strategies ---%s\n", COL_BOLD, COL_RESET);
    fflush(stdout);
    {
        /* 1. DPDK BusySpin */
        dpdk_ring_t *r = dpdk_ring_alloc(RING_CAPACITY);
        dpdk_ring_t *r_ba = dpdk_ring_alloc(RING_CAPACITY);
        dpdk_ring_reset(r);
        double tput = run_dpdk_tput(r, 0);
        dpdk_ring_reset(r); dpdk_ring_reset(r_ba);
        run_dpdk_lat(r, r_ba, 0, lat_buf);
        dpdk_ring_reset(r);
        run_dpdk_wake(r, 0, wake_buf);
        dpdk_ring_reset(r);
        long vol = 0, invol = 0;
        double cpu = measure_dpdk_idle_cpu(r, 0, &vol, &invol);
        record_res(results, &n_res, "DPDK-BusySpin", "DPDK Polling", tput, lat_buf, wake_buf, cpu, vol, invol);
        dpdk_ring_free(r);
        dpdk_ring_free(r_ba);

        /* 2. DPDK PauseLoop */
        r = dpdk_ring_alloc(RING_CAPACITY);
        r_ba = dpdk_ring_alloc(RING_CAPACITY);
        dpdk_ring_reset(r);
        tput = run_dpdk_tput(r, 1);
        dpdk_ring_reset(r); dpdk_ring_reset(r_ba);
        run_dpdk_lat(r, r_ba, 1, lat_buf);
        dpdk_ring_reset(r);
        run_dpdk_wake(r, 1, wake_buf);
        dpdk_ring_reset(r);
        cpu = measure_dpdk_idle_cpu(r, 1, &vol, &invol);
        record_res(results, &n_res, "DPDK-PauseLoop", "DPDK Power", tput, lat_buf, wake_buf, cpu, vol, invol);
        dpdk_ring_free(r);
        dpdk_ring_free(r_ba);

        /* 3. DPDK Interrupt-Epoll */
        r = dpdk_ring_alloc(RING_CAPACITY);
        r_ba = dpdk_ring_alloc(RING_CAPACITY);
        dpdk_ring_reset(r);
        tput = run_dpdk_tput(r, 2);
        dpdk_ring_reset(r); dpdk_ring_reset(r_ba);
        run_dpdk_lat(r, r_ba, 2, lat_buf);
        dpdk_ring_reset(r);
        run_dpdk_wake(r, 2, wake_buf);
        dpdk_ring_reset(r);
        cpu = measure_dpdk_idle_cpu(r, 2, &vol, &invol);
        record_res(results, &n_res, "DPDK-Interrupt-Epoll", "DPDK Interrupt", tput, lat_buf, wake_buf, cpu, vol, invol);
        dpdk_ring_free(r);
        dpdk_ring_free(r_ba);
    }

    printf("\n%s--- Benchmarking LMAX Disruptor Strategies ---%s\n", COL_BOLD, COL_RESET);
    fflush(stdout);
    {
        /* 4. LMAX BusySpin */
        lmax_ring_t *d = lmax_ring_alloc(RING_CAPACITY);
        lmax_ring_t *d_ba = lmax_ring_alloc(RING_CAPACITY);
        lmax_ring_reset(d);
        double tput = run_lmax_tput(d, LMAX_STRAT_BUSYSPIN);
        lmax_ring_reset(d); lmax_ring_reset(d_ba);
        run_lmax_lat(d, d_ba, LMAX_STRAT_BUSYSPIN, lat_buf);
        lmax_ring_reset(d);
        run_lmax_wake(d, LMAX_STRAT_BUSYSPIN, wake_buf);
        lmax_ring_reset(d);
        long vol = 0, invol = 0;
        double cpu = measure_lmax_idle_cpu(d, LMAX_STRAT_BUSYSPIN, &vol, &invol);
        record_res(results, &n_res, "LMAX-BusySpin", "Disruptor Spin", tput, lat_buf, wake_buf, cpu, vol, invol);
        lmax_ring_free(d);
        lmax_ring_free(d_ba);

        /* 5. LMAX Yielding */
        d = lmax_ring_alloc(RING_CAPACITY);
        d_ba = lmax_ring_alloc(RING_CAPACITY);
        lmax_ring_reset(d);
        tput = run_lmax_tput(d, LMAX_STRAT_YIELDING);
        lmax_ring_reset(d); lmax_ring_reset(d_ba);
        run_lmax_lat(d, d_ba, LMAX_STRAT_YIELDING, lat_buf);
        lmax_ring_reset(d);
        run_lmax_wake(d, LMAX_STRAT_YIELDING, wake_buf);
        lmax_ring_reset(d);
        cpu = measure_lmax_idle_cpu(d, LMAX_STRAT_YIELDING, &vol, &invol);
        record_res(results, &n_res, "LMAX-Yielding", "Disruptor Yield", tput, lat_buf, wake_buf, cpu, vol, invol);
        lmax_ring_free(d);
        lmax_ring_free(d_ba);

        /* 6. LMAX Sleeping */
        d = lmax_ring_alloc(RING_CAPACITY);
        d_ba = lmax_ring_alloc(RING_CAPACITY);
        lmax_ring_reset(d);
        tput = run_lmax_tput(d, LMAX_STRAT_SLEEPING);
        lmax_ring_reset(d); lmax_ring_reset(d_ba);
        run_lmax_lat(d, d_ba, LMAX_STRAT_SLEEPING, lat_buf);
        lmax_ring_reset(d);
        run_lmax_wake(d, LMAX_STRAT_SLEEPING, wake_buf);
        lmax_ring_reset(d);
        cpu = measure_lmax_idle_cpu(d, LMAX_STRAT_SLEEPING, &vol, &invol);
        record_res(results, &n_res, "LMAX-Sleeping", "Disruptor Sleep", tput, lat_buf, wake_buf, cpu, vol, invol);
        lmax_ring_free(d);
        lmax_ring_free(d_ba);

        /* 7. LMAX Blocking */
        d = lmax_ring_alloc(RING_CAPACITY);
        d_ba = lmax_ring_alloc(RING_CAPACITY);
        lmax_ring_reset(d);
        tput = run_lmax_tput(d, LMAX_STRAT_BLOCKING);
        lmax_ring_reset(d); lmax_ring_reset(d_ba);
        run_lmax_lat(d, d_ba, LMAX_STRAT_BLOCKING, lat_buf);
        lmax_ring_reset(d);
        run_lmax_wake(d, LMAX_STRAT_BLOCKING, wake_buf);
        lmax_ring_reset(d);
        cpu = measure_lmax_idle_cpu(d, LMAX_STRAT_BLOCKING, &vol, &invol);
        record_res(results, &n_res, "LMAX-Blocking", "Disruptor Mutex", tput, lat_buf, wake_buf, cpu, vol, invol);
        lmax_ring_free(d);
        lmax_ring_free(d_ba);

        /* 8. LMAX LiteBlocking */
        d = lmax_ring_alloc(RING_CAPACITY);
        d_ba = lmax_ring_alloc(RING_CAPACITY);
        lmax_ring_reset(d);
        tput = run_lmax_tput(d, LMAX_STRAT_LITEBLOCKING);
        lmax_ring_reset(d); lmax_ring_reset(d_ba);
        run_lmax_lat(d, d_ba, LMAX_STRAT_LITEBLOCKING, lat_buf);
        lmax_ring_reset(d);
        run_lmax_wake(d, LMAX_STRAT_LITEBLOCKING, wake_buf);
        lmax_ring_reset(d);
        cpu = measure_lmax_idle_cpu(d, LMAX_STRAT_LITEBLOCKING, &vol, &invol);
        record_res(results, &n_res, "LMAX-LiteBlocking", "Disruptor Lite", tput, lat_buf, wake_buf, cpu, vol, invol);
        lmax_ring_free(d);
        lmax_ring_free(d_ba);

        /* 9. LMAX PhasedBackoff */
        d = lmax_ring_alloc(RING_CAPACITY);
        d_ba = lmax_ring_alloc(RING_CAPACITY);
        lmax_ring_reset(d);
        tput = run_lmax_tput(d, LMAX_STRAT_PHASEDBACKOFF);
        lmax_ring_reset(d); lmax_ring_reset(d_ba);
        run_lmax_lat(d, d_ba, LMAX_STRAT_PHASEDBACKOFF, lat_buf);
        lmax_ring_reset(d);
        run_lmax_wake(d, LMAX_STRAT_PHASEDBACKOFF, wake_buf);
        lmax_ring_reset(d);
        cpu = measure_lmax_idle_cpu(d, LMAX_STRAT_PHASEDBACKOFF, &vol, &invol);
        record_res(results, &n_res, "LMAX-PhasedBackoff", "Disruptor Phased", tput, lat_buf, wake_buf, cpu, vol, invol);
        lmax_ring_free(d);
        lmax_ring_free(d_ba);
    }

    printf("\n%s--- Benchmarking librbipc Strategy ---%s\n", COL_BOLD, COL_RESET);
    fflush(stdout);
    {
        /* 10. librbipc Futex */
        const char *ab_name = "/rbench_idle_ab";
        const char *ba_name = "/rbench_idle_ba";
        rbipc_env_t env = rbipc_init(ab_name, ba_name);
        double tput = run_rbipc_tput(&env);
        run_rbipc_lat(&env, lat_buf);
        run_rbipc_wake(&env, wake_buf);
        long vol = 0, invol = 0;
        double cpu = measure_rbipc_idle_cpu(&env, &vol, &invol);
        record_res(results, &n_res, "librbipc-Futex", "Adaptive Futex", tput, lat_buf, wake_buf, cpu, vol, invol);
        rbipc_cleanup(&env, ab_name, ba_name);
    }

    /* Print Comparison Table */
    printf("\n%s%s========================================================================================================================%s\n",
           COL_BOLD, COL_CYAN, COL_RESET);
    printf("%s%-22s | %-15s | %10s | %9s | %9s | %9s | %10s | %7s%s\n",
           COL_BOLD, "Strategy Name", "Category", "Throughput", "Lat p50", "Lat p99", "Wake p50", "Wake p99", "Idle CPU", COL_RESET);
    printf("%s-----------------------+-----------------+------------+-----------+-----------+-----------+------------+--------%s\n",
           COL_CYAN, COL_RESET);

    for (size_t i = 0; i < n_res; i++) {
        idle_bench_result_t *r = &results[i];
        printf("%-22s | %-15s | %6.2f Mop/s | %6lu ns | %6lu ns | %6lu ns | %7lu ns | %5.1f%%\n",
               r->strategy_name, r->category, r->tput_mops, r->lat_p50, r->lat_p99, r->wake_p50, r->wake_p99, r->idle_cpu_pct);
    }
    printf("%s========================================================================================================================%s\n\n",
           COL_CYAN, COL_RESET);
    fflush(stdout);

    /* Output JSON */
    char json_path[256];
    bench_get_result_path(json_path, sizeof(json_path), "idle_strategies_benchmark.json");
    FILE *fp = fopen(json_path, "w");
    if (fp) {
        fprintf(fp, "{\n");
        fprintf(fp, "  \"timestamp\": %lu,\n", get_ns());
        fprintf(fp, "  \"results\": [\n");
        for (size_t i = 0; i < n_res; i++) {
            idle_bench_result_t *r = &results[i];
            fprintf(fp, "    {\n");
            fprintf(fp, "      \"strategy\": \"%s\",\n", r->strategy_name);
            fprintf(fp, "      \"category\": \"%s\",\n", r->category);
            fprintf(fp, "      \"throughput_mops\": %.4f,\n", r->tput_mops);
            fprintf(fp, "      \"throughput_mbs\": %.2f,\n", r->tput_mbs);
            fprintf(fp, "      \"lat_p50_ns\": %lu,\n", r->lat_p50);
            fprintf(fp, "      \"lat_p90_ns\": %lu,\n", r->lat_p90);
            fprintf(fp, "      \"lat_p99_ns\": %lu,\n", r->lat_p99);
            fprintf(fp, "      \"lat_p999_ns\": %lu,\n", r->lat_p999);
            fprintf(fp, "      \"wake_p50_ns\": %lu,\n", r->wake_p50);
            fprintf(fp, "      \"wake_p90_ns\": %lu,\n", r->wake_p90);
            fprintf(fp, "      \"wake_p99_ns\": %lu,\n", r->wake_p99);
            fprintf(fp, "      \"wake_max_ns\": %lu,\n", r->wake_max);
            fprintf(fp, "      \"idle_cpu_pct\": %.2f,\n", r->idle_cpu_pct);
            fprintf(fp, "      \"voluntary_ctx_switches\": %ld,\n", r->voluntary_ctx);
            fprintf(fp, "      \"involuntary_ctx_switches\": %ld\n", r->involuntary_ctx);
            fprintf(fp, "    }%s\n", (i + 1 < n_res) ? "," : "");
        }
        fprintf(fp, "  ]\n");
        fprintf(fp, "}\n");
        fclose(fp);
        printf("%s[INFO]%s JSON output saved to %s\n",
               COL_GREEN, COL_RESET, json_path);
    }

    free(lat_buf);
    free(wake_buf);
    return 0;
}
