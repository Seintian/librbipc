/**
 * @file idle_disruptor.c
 * @brief LMAX Disruptor wait strategies implementation for idle benchmark.
 */

#include "idle_common.h"

struct lmax_ring {
    alignas(CACHE_LINE_SIZE) _Atomic int64_t prod_cursor;
    alignas(CACHE_LINE_SIZE) _Atomic int64_t prod_claim;
    alignas(CACHE_LINE_SIZE) _Atomic int64_t work_sequence;
    alignas(CACHE_LINE_SIZE) _Atomic int64_t cons_gating;
    uint32_t capacity;
    uint32_t mask;

    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    alignas(CACHE_LINE_SIZE) atomic_bool signal_needed;
};

lmax_ring_t *lmax_ring_alloc(uint32_t cap) {
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

void lmax_ring_reset(lmax_ring_t *d) {
    if (!d) return;
    atomic_store_explicit(&d->prod_cursor, -1, memory_order_relaxed);
    atomic_store_explicit(&d->prod_claim, -1, memory_order_relaxed);
    atomic_store_explicit(&d->work_sequence, -1, memory_order_relaxed);
    atomic_store_explicit(&d->cons_gating, -1, memory_order_relaxed);
    atomic_store_explicit(&d->signal_needed, false, memory_order_relaxed);
}

void lmax_ring_free(lmax_ring_t *d) {
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
        idle_cpu_pause();
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
                idle_cpu_pause();
            }
            break;

        case LMAX_STRAT_YIELDING: {
            int counter = 100;
            while (atomic_load_explicit(&d->prod_cursor, memory_order_acquire) < next) {
                if (counter == 0) {
                    sched_yield();
                } else {
                    counter--;
                    idle_cpu_pause();
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
                    idle_cpu_pause();
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
            uint64_t t0 = idle_get_ns();
            bool done = false;
            while (!done) {
                if (atomic_load_explicit(&d->prod_cursor, memory_order_acquire) >= next) {
                    done = true;
                    break;
                }
                uint64_t delta = idle_get_ns() - t0;
                if (delta < 1000) {
                    idle_cpu_pause();
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

/* Throughput */
typedef struct {
    lmax_ring_t *d;
    lmax_strat_t strat;
    atomic_bool *start;
    atomic_bool *done;
} lmax_th_arg_t;

static void *lmax_tput_worker(void *p) {
    lmax_th_arg_t *a = (lmax_th_arg_t *)p;
    while (!atomic_load_explicit(a->start, memory_order_acquire)) idle_cpu_pause();
    uintptr_t val;
    for (uint64_t i = 0; i < g_tput_iters; i++) {
        lmax_consume(a->d, (int64_t)g_tput_iters, &val, a->strat);
    }
    atomic_store_explicit(a->done, true, memory_order_release);
    return NULL;
}

double run_lmax_tput(lmax_ring_t *d, lmax_strat_t strat) {
    atomic_bool start = false, done = false;
    lmax_th_arg_t arg = { d, strat, &start, &done };

    pthread_t th;
    pthread_create(&th, NULL, lmax_tput_worker, &arg);

    uint64_t t0 = idle_get_ns();
    atomic_store_explicit(&start, true, memory_order_release);
    for (uint64_t i = 0; i < g_tput_iters; i++) {
        lmax_publish(d, (int64_t)i, (uintptr_t)i, strat);
    }
    pthread_join(th, NULL);
    uint64_t t1 = idle_get_ns();

    double sec = (double)(t1 - t0) / 1e9;
    return (double)g_tput_iters / sec;
}

/* Latency */
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

void run_lmax_lat(lmax_ring_t *ab, lmax_ring_t *ba, lmax_strat_t strat, uint64_t *samples) {
    atomic_bool stop = false;
    lmax_lat_arg_t arg = { ab, ba, strat, &stop };

    pthread_t th;
    pthread_create(&th, NULL, lmax_lat_worker, &arg);

    uintptr_t resp;
    for (uint64_t i = 0; i < g_lat_iters; i++) {
        uint64_t t0 = idle_get_ns();
        lmax_publish(ab, (int64_t)i, (uintptr_t)i, strat);
        lmax_consume(ba, -1, &resp, strat);
        uint64_t t1 = idle_get_ns();
        samples[i] = (t1 - t0) / 2;
    }
    atomic_store_explicit(&stop, true, memory_order_release);
    lmax_publish(ab, (int64_t)g_lat_iters, UINTPTR_MAX, strat);
    pthread_join(th, NULL);
}

/* Wakeup */
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
        uint64_t now = idle_get_ns();
        if (val == UINTPTR_MAX) break;
        atomic_store_explicit(a->recv_ts, now, memory_order_release);
    }
    return NULL;
}

void run_lmax_wake(lmax_ring_t *d, lmax_strat_t strat, uint64_t *wake_samples) {
    atomic_bool stop = false;
    alignas(CACHE_LINE_SIZE) _Atomic uint64_t recv_ts = 0;
    lmax_wake_arg_t arg = { d, strat, &stop, &recv_ts };

    pthread_t th;
    pthread_create(&th, NULL, lmax_wake_worker, &arg);

    struct timespec sleep_gap = {
        .tv_sec = (time_t)(g_wake_gap_ns / 1000000000ULL),
        .tv_nsec = (long)(g_wake_gap_ns % 1000000000ULL)
    };
    for (uint64_t i = 0; i < g_wake_iters; i++) {
        nanosleep(&sleep_gap, NULL);
        atomic_store_explicit(&recv_ts, 0, memory_order_relaxed);
        uint64_t send_ts = idle_get_ns();
        lmax_publish(d, (int64_t)i, (uintptr_t)i, strat);
        uint32_t spins = 0;
        while (atomic_load_explicit(&recv_ts, memory_order_acquire) == 0) bench_spin_pause(&spins);
        uint64_t rts = atomic_load_explicit(&recv_ts, memory_order_acquire);
        wake_samples[i] = (rts > send_ts) ? (rts - send_ts) : 0;
    }

    atomic_store_explicit(&stop, true, memory_order_release);
    lmax_publish(d, (int64_t)g_wake_iters, UINTPTR_MAX, strat);
    pthread_join(th, NULL);
}

/* Idle CPU */
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
    while (!atomic_load_explicit(a->start, memory_order_acquire)) idle_cpu_pause();
    uint64_t cpu0 = idle_get_thread_cpu_ns();
    struct rusage ru0;
    getrusage(RUSAGE_THREAD, &ru0);

    while (!atomic_load_explicit(a->stop, memory_order_acquire)) {
        if (a->strat == LMAX_STRAT_BUSYSPIN) {
            idle_cpu_pause();
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
    uint64_t cpu1 = idle_get_thread_cpu_ns();
    struct rusage ru1;
    getrusage(RUSAGE_THREAD, &ru1);
    atomic_store_explicit(a->cpu_time_spent, (cpu1 - cpu0), memory_order_release);
    *(a->vol_ctx) = ru1.ru_nvcsw - ru0.ru_nvcsw;
    *(a->invol_ctx) = ru1.ru_nivcsw - ru0.ru_nivcsw;
    return NULL;
}

double measure_lmax_idle_cpu(lmax_ring_t *d, lmax_strat_t strat, long *vol_ctx, long *invol_ctx) {
    atomic_bool start = false, stop = false;
    alignas(CACHE_LINE_SIZE) _Atomic uint64_t cpu_time_spent = 0;
    lmax_idle_arg_t arg = { d, strat, &start, &stop, &cpu_time_spent, vol_ctx, invol_ctx };

    pthread_t th;
    pthread_create(&th, NULL, lmax_idle_worker, &arg);

    uint64_t w0 = idle_get_ns();
    atomic_store_explicit(&start, true, memory_order_release);
    struct timespec sleep_sec = {
        .tv_sec = (time_t)(g_idle_obs_ns / 1000000000ULL),
        .tv_nsec = (long)(g_idle_obs_ns % 1000000000ULL)
    };
    nanosleep(&sleep_sec, NULL);
    atomic_store_explicit(&stop, true, memory_order_release);

    pthread_join(th, NULL);
    uint64_t w1 = idle_get_ns();

    double wall_ns = (double)(w1 - w0);
    double cpu_ns  = (double)atomic_load_explicit(&cpu_time_spent, memory_order_acquire);
    return (cpu_ns / wall_ns) * 100.0;
}
