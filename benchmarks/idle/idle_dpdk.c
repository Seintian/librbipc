/**
 * @file idle_dpdk.c
 * @brief DPDK rte_ring implementation and wait strategies for idle benchmark.
 */

#include "idle_common.h"

struct dpdk_ring {
    alignas(CACHE_LINE_SIZE) atomic_uint prod_head;
    alignas(CACHE_LINE_SIZE) atomic_uint prod_tail;
    alignas(CACHE_LINE_SIZE) atomic_uint cons_head;
    alignas(CACHE_LINE_SIZE) atomic_uint cons_tail;
    uint32_t mask;
    uint32_t capacity;

    int efd;
    int epfd;
    alignas(CACHE_LINE_SIZE) atomic_bool in_sleep;
};

dpdk_ring_t *dpdk_ring_alloc(uint32_t cap) {
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

void dpdk_ring_reset(dpdk_ring_t *r) {
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

void dpdk_ring_free(dpdk_ring_t *r) {
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
        idle_cpu_pause();
    atomic_store_explicit(&r->prod_tail, pn, memory_order_release);

    if (atomic_load_explicit(&r->in_sleep, memory_order_seq_cst)) {
        uint64_t one = 1;
        ssize_t w = write(r->efd, &one, sizeof(one));
        (void)w;
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
        idle_cpu_pause();
    atomic_store_explicit(&r->cons_tail, cn, memory_order_release);
    return 0;
}

static void dpdk_dequeue_busyspin(dpdk_ring_t *r, uintptr_t *out) {
    while (dpdk_try_dequeue(r, out) == -ENOENT) {
        idle_cpu_pause();
    }
}

static void dpdk_dequeue_pauseloop(dpdk_ring_t *r, uintptr_t *out) {
    uint32_t empty_polls = 0;
    while (dpdk_try_dequeue(r, out) == -ENOENT) {
        empty_polls++;
        if (empty_polls > 64) {
            for (int i = 0; i < 50; i++) idle_cpu_pause();
        } else {
            idle_cpu_pause();
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
                ssize_t rret = read(r->efd, &val, sizeof(val));
                (void)rret;
            }
            atomic_store_explicit(&r->in_sleep, false, memory_order_seq_cst);
            empty_polls = 0;
        } else {
            idle_cpu_pause();
        }
    }
}

/* Throughput */
typedef struct {
    dpdk_ring_t *r;
    int strat;
    atomic_bool *start;
    atomic_bool *done;
} dpdk_th_arg_t;

static void *dpdk_tput_worker(void *p) {
    dpdk_th_arg_t *a = (dpdk_th_arg_t *)p;
    while (!atomic_load_explicit(a->start, memory_order_acquire)) idle_cpu_pause();
    uintptr_t val;
    for (uint64_t i = 0; i < g_tput_iters; i++) {
        if (a->strat == 0) dpdk_dequeue_busyspin(a->r, &val);
        else if (a->strat == 1) dpdk_dequeue_pauseloop(a->r, &val);
        else dpdk_dequeue_interrupt(a->r, &val);
    }
    atomic_store_explicit(a->done, true, memory_order_release);
    return NULL;
}

double run_dpdk_tput(dpdk_ring_t *r, int strat_mode) {
    atomic_bool start = false, done = false;
    dpdk_th_arg_t arg = { r, strat_mode, &start, &done };

    pthread_t th;
    pthread_create(&th, NULL, dpdk_tput_worker, &arg);

    for (uint64_t i = 0; i < g_tput_warmup; i++) {
        while (dpdk_try_enqueue(r, (uintptr_t)i) == -ENOBUFS) idle_cpu_pause();
        uintptr_t tmp;
        while (dpdk_try_dequeue(r, &tmp) == -ENOENT) idle_cpu_pause();
    }

    uint64_t t0 = idle_get_ns();
    atomic_store_explicit(&start, true, memory_order_release);
    for (uint64_t i = 0; i < g_tput_iters; i++) {
        while (dpdk_try_enqueue(r, (uintptr_t)i) == -ENOBUFS) idle_cpu_pause();
    }
    pthread_join(th, NULL);
    uint64_t t1 = idle_get_ns();

    double sec = (double)(t1 - t0) / 1e9;
    return (double)g_tput_iters / sec;
}

/* Latency */
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
        while (dpdk_try_enqueue(a->ba, val) == -ENOBUFS) idle_cpu_pause();
    }
    return NULL;
}

void run_dpdk_lat(dpdk_ring_t *ab, dpdk_ring_t *ba, int strat, uint64_t *samples) {
    atomic_bool stop = false;
    dpdk_lat_arg_t arg = { ab, ba, strat, &stop };

    pthread_t th;
    pthread_create(&th, NULL, dpdk_lat_worker, &arg);

    uintptr_t resp;
    for (uint64_t i = 0; i < g_lat_iters; i++) {
        uint64_t t0 = idle_get_ns();
        while (dpdk_try_enqueue(ab, (uintptr_t)i) == -ENOBUFS) idle_cpu_pause();
        if (strat == 0) dpdk_dequeue_busyspin(ba, &resp);
        else if (strat == 1) dpdk_dequeue_pauseloop(ba, &resp);
        else dpdk_dequeue_interrupt(ba, &resp);
        uint64_t t1 = idle_get_ns();
        samples[i] = (t1 - t0) / 2;
    }
    atomic_store_explicit(&stop, true, memory_order_release);
    while (dpdk_try_enqueue(ab, UINTPTR_MAX) == -ENOBUFS) idle_cpu_pause();
    pthread_join(th, NULL);
}

/* Wakeup */
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
        uint64_t now = idle_get_ns();
        if (val == UINTPTR_MAX) break;
        atomic_store_explicit(a->recv_ts, now, memory_order_release);
    }
    return NULL;
}

void run_dpdk_wake(dpdk_ring_t *r, int strat, uint64_t *wake_samples) {
    atomic_bool stop = false;
    alignas(CACHE_LINE_SIZE) _Atomic uint64_t recv_ts = 0;
    dpdk_wake_arg_t arg = { r, strat, &stop, &recv_ts };

    pthread_t th;
    pthread_create(&th, NULL, dpdk_wake_worker, &arg);

    struct timespec sleep_gap = {
        .tv_sec = (time_t)(g_wake_gap_ns / 1000000000ULL),
        .tv_nsec = (long)(g_wake_gap_ns % 1000000000ULL)
    };
    for (uint64_t i = 0; i < g_wake_iters; i++) {
        nanosleep(&sleep_gap, NULL);
        atomic_store_explicit(&recv_ts, 0, memory_order_relaxed);
        uint64_t send_ts = idle_get_ns();
        uint32_t spins = 0;
        while (dpdk_try_enqueue(r, (uintptr_t)i) == -ENOBUFS) bench_spin_pause(&spins);
        spins = 0;
        while (atomic_load_explicit(&recv_ts, memory_order_acquire) == 0) bench_spin_pause(&spins);
        uint64_t rts = atomic_load_explicit(&recv_ts, memory_order_acquire);
        wake_samples[i] = (rts > send_ts) ? (rts - send_ts) : 0;
    }

    atomic_store_explicit(&stop, true, memory_order_release);
    while (dpdk_try_enqueue(r, UINTPTR_MAX) == -ENOBUFS) idle_cpu_pause();
    pthread_join(th, NULL);
}

/* Idle CPU */
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
    while (!atomic_load_explicit(a->start, memory_order_acquire)) idle_cpu_pause();
    uint64_t cpu0 = idle_get_thread_cpu_ns();
    struct rusage ru0;
    getrusage(RUSAGE_THREAD, &ru0);

    while (!atomic_load_explicit(a->stop, memory_order_acquire)) {
        if (a->strat == 0) {
            idle_cpu_pause();
        } else if (a->strat == 1) {
            for (int i = 0; i < 50; i++) idle_cpu_pause();
        } else {
            struct epoll_event ev;
            (void)epoll_wait(a->r->epfd, &ev, 1, 10);
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

double measure_dpdk_idle_cpu(dpdk_ring_t *r, int strat, long *vol_ctx, long *invol_ctx) {
    atomic_bool start = false, stop = false;
    alignas(CACHE_LINE_SIZE) _Atomic uint64_t cpu_time_spent = 0;
    dpdk_idle_arg_t arg = { r, strat, &start, &stop, &cpu_time_spent, vol_ctx, invol_ctx };

    pthread_t th;
    pthread_create(&th, NULL, dpdk_idle_worker, &arg);

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
