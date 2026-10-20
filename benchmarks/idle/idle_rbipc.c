/**
 * @file idle_rbipc.c
 * @brief librbipc wait strategy implementation for idle benchmark.
 */

#include "idle_common.h"

rbipc_env_t rbipc_env_init(const char *name_ab, const char *name_ba) {
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
    rbipc_set_wait_policy(env.ring_ab_prod, RBIPC_WAIT_ADAPTIVE);
    rbipc_set_wait_policy(env.ring_ab_cons, RBIPC_WAIT_ADAPTIVE);
    rbipc_set_wait_policy(env.ring_ba_prod, RBIPC_WAIT_ADAPTIVE);
    rbipc_set_wait_policy(env.ring_ba_cons, RBIPC_WAIT_ADAPTIVE);
    return env;
}

void rbipc_env_cleanup(rbipc_env_t *env, const char *name_ab, const char *name_ba) {
    if (env->ring_ab_prod) rbipc_detach(env->ring_ab_prod);
    if (env->ring_ab_cons) rbipc_detach(env->ring_ab_cons);
    if (env->ring_ba_prod) rbipc_detach(env->ring_ba_prod);
    if (env->ring_ba_cons) rbipc_detach(env->ring_ba_cons);
    rbipc_destroy(name_ab);
    rbipc_destroy(name_ba);
}

typedef struct {
    rbipc_ring_t *r;
    atomic_bool *start;
    atomic_bool *done;
} rbipc_th_arg_t;

static void *rbipc_tput_worker(void *p) {
    rbipc_th_arg_t *a = (rbipc_th_arg_t *)p;
    while (!atomic_load_explicit(a->start, memory_order_acquire)) idle_cpu_pause();
    const void *buf;
    uint32_t len;
    uint64_t ticket;
    for (uint64_t i = 0; i < g_tput_iters; i++) {
        while (rbipc_read_acquire(a->r, &buf, &len, &ticket) != RBIPC_OK) idle_cpu_pause();
        rbipc_read_release(a->r, ticket);
    }
    atomic_store_explicit(a->done, true, memory_order_release);
    return NULL;
}

double run_rbipc_tput(rbipc_env_t *env) {
    atomic_bool start = false, done = false;
    rbipc_th_arg_t arg = { env->ring_ab_cons, &start, &done };

    pthread_t th;
    pthread_create(&th, NULL, rbipc_tput_worker, &arg);

    uint64_t t0 = idle_get_ns();
    atomic_store_explicit(&start, true, memory_order_release);
    void *wbuf;
    uint64_t wtick;
    for (uint64_t i = 0; i < g_tput_iters; i++) {
        while (rbipc_reserve_write(env->ring_ab_prod, MSG_PAYLOAD_SIZE, &wbuf, &wtick) != RBIPC_OK) idle_cpu_pause();
        rbipc_commit_write(env->ring_ab_prod, wtick, MSG_PAYLOAD_SIZE);
    }
    pthread_join(th, NULL);
    uint64_t t1 = idle_get_ns();

    double sec = (double)(t1 - t0) / 1e9;
    return (double)g_tput_iters / sec;
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
            while (rbipc_reserve_write(a->ba, rlen, &wbuf, &wtick) != RBIPC_OK) idle_cpu_pause();
            rbipc_commit_write(a->ba, wtick, rlen);
            rbipc_read_release(a->ab, rtick);
        }
    }
    return NULL;
}

void run_rbipc_lat(rbipc_env_t *env, uint64_t *samples) {
    atomic_bool stop = false;
    rbipc_lat_arg_t arg = { env->ring_ab_cons, env->ring_ba_prod, &stop };

    pthread_t th;
    pthread_create(&th, NULL, rbipc_lat_worker, &arg);

    const void *rbuf;
    uint32_t rlen;
    uint64_t rtick;
    void *wbuf;
    uint64_t wtick;

    for (uint64_t i = 0; i < g_lat_iters; i++) {
        uint64_t t0 = idle_get_ns();
        while (rbipc_reserve_write(env->ring_ab_prod, MSG_PAYLOAD_SIZE, &wbuf, &wtick) != RBIPC_OK) idle_cpu_pause();
        rbipc_commit_write(env->ring_ab_prod, wtick, MSG_PAYLOAD_SIZE);
        while (rbipc_read_acquire(env->ring_ba_cons, &rbuf, &rlen, &rtick) != RBIPC_OK) idle_cpu_pause();
        rbipc_read_release(env->ring_ba_cons, rtick);
        uint64_t t1 = idle_get_ns();
        samples[i] = (t1 - t0) / 2;
    }
    atomic_store_explicit(&stop, true, memory_order_release);
    while (rbipc_reserve_write(env->ring_ab_prod, 0, &wbuf, &wtick) != RBIPC_OK) idle_cpu_pause();
    rbipc_commit_write(env->ring_ab_prod, wtick, 0);
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
            uint64_t now = idle_get_ns();
            if (len == 0) { rbipc_read_release(a->r, ticket); break; }
            atomic_store_explicit(a->recv_ts, now, memory_order_release);
            rbipc_read_release(a->r, ticket);
        }
    }
    return NULL;
}

void run_rbipc_wake(rbipc_env_t *env, uint64_t *wake_samples) {
    atomic_bool stop = false;
    alignas(CACHE_LINE_SIZE) _Atomic uint64_t recv_ts = 0;
    rbipc_wake_arg_t arg = { env->ring_ab_cons, &stop, &recv_ts };

    pthread_t th;
    pthread_create(&th, NULL, rbipc_wake_worker, &arg);

    void *wbuf;
    uint64_t wtick;
    struct timespec sleep_gap = {
        .tv_sec = (time_t)(g_wake_gap_ns / 1000000000ULL),
        .tv_nsec = (long)(g_wake_gap_ns % 1000000000ULL)
    };
    for (uint64_t i = 0; i < g_wake_iters; i++) {
        nanosleep(&sleep_gap, NULL);
        atomic_store_explicit(&recv_ts, 0, memory_order_relaxed);
        uint64_t send_ts = idle_get_ns();
        uint32_t spins = 0;
        while (rbipc_reserve_write(env->ring_ab_prod, MSG_PAYLOAD_SIZE, &wbuf, &wtick) != RBIPC_OK) bench_spin_pause(&spins);
        rbipc_commit_write(env->ring_ab_prod, wtick, MSG_PAYLOAD_SIZE);
        spins = 0;
        while (atomic_load_explicit(&recv_ts, memory_order_acquire) == 0) bench_spin_pause(&spins);
        uint64_t rts = atomic_load_explicit(&recv_ts, memory_order_acquire);
        wake_samples[i] = (rts > send_ts) ? (rts - send_ts) : 0;
    }

    atomic_store_explicit(&stop, true, memory_order_release);
    while (rbipc_reserve_write(env->ring_ab_prod, 0, &wbuf, &wtick) != RBIPC_OK) idle_cpu_pause();
    rbipc_commit_write(env->ring_ab_prod, wtick, 0);
    pthread_join(th, NULL);
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
    while (!atomic_load_explicit(a->start, memory_order_acquire)) idle_cpu_pause();
    uint64_t cpu0 = idle_get_thread_cpu_ns();
    struct rusage ru0;
    getrusage(RUSAGE_THREAD, &ru0);

    while (!atomic_load_explicit(a->stop, memory_order_acquire)) {
        int rc = rbipc_read_acquire_timeout(a->r, 50000000ULL, &buf, &len, &ticket);
        if (rc == RBIPC_OK) {
            rbipc_read_release(a->r, ticket);
            break;
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

double measure_rbipc_idle_cpu(rbipc_env_t *env, long *vol_ctx, long *invol_ctx) {
    atomic_bool start = false, stop = false;
    alignas(CACHE_LINE_SIZE) _Atomic uint64_t cpu_time_spent = 0;
    rbipc_idle_arg_t arg = { env->ring_ab_cons, &start, &stop, &cpu_time_spent, vol_ctx, invol_ctx };

    pthread_t th;
    pthread_create(&th, NULL, rbipc_idle_worker, &arg);

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
