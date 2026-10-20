/**
 * @file suite_mpmc.c
 * @brief Multi-threaded MPMC scalability benchmark
 */

#include "suite_common.h"

typedef struct {
    rbipc_ring_t *ring;
    uint32_t count;
} thread_arg_t;

static _Atomic uint32_t g_mpmc_consumed;

static void *mpmc_prod_thread(void *arg) {
    thread_arg_t *targ = (thread_arg_t *)arg;
    rbipc_ring_t *ring = targ->ring;
    uint32_t count = targ->count;

    for (uint32_t i = 0; i < count; ++i) {
        void *buf = NULL;
        uint64_t ticket = 0;
        int rc = rbipc_reserve_write(ring, 64, &buf, &ticket);
        assert(rc == RBIPC_OK);
        *(uint32_t *)buf = i;
        rc = rbipc_commit_write(ring, ticket, 64);
        assert(rc == RBIPC_OK);
    }
    return NULL;
}

static void *mpmc_cons_thread(void *arg) {
    thread_arg_t *targ = (thread_arg_t *)arg;
    rbipc_ring_t *ring = targ->ring;
    uint32_t total = targ->count;

    while (atomic_load_explicit(&g_mpmc_consumed, memory_order_relaxed) < total) {
        const void *buf = NULL;
        uint32_t len = 0;
        uint64_t ticket = 0;
        int rc = rbipc_read_acquire_timeout(ring, 50000000ULL, &buf, &len, &ticket);
        if (rc == RBIPC_ERR_TIMEOUT || rc == RBIPC_ERR_SHUTDOWN) {
            if (atomic_load_explicit(&g_mpmc_consumed, memory_order_relaxed) >= total) break;
            continue;
        }
        assert(rc == RBIPC_OK);
        atomic_fetch_add_explicit(&g_mpmc_consumed, 1, memory_order_relaxed);
        rbipc_read_release(ring, ticket);
    }
    return NULL;
}

void run_bench_mpmc(uint32_t np, uint32_t nc, uint32_t total, mpmc_result_t *out_res) {
    atomic_init(&g_mpmc_consumed, 0);

    rbipc_ring_t *ring = NULL;
    assert(rbipc_create(NULL, 1024, 64, &ring) == RBIPC_OK);

    pthread_t prods[np];
    pthread_t cons[nc];
    thread_arg_t parg = { .ring = ring, .count = total / np };
    thread_arg_t carg = { .ring = ring, .count = total };

    uint64_t t0 = get_time_ns();

    for (uint32_t i = 0; i < nc; ++i) {
        pthread_create(&cons[i], NULL, mpmc_cons_thread, &carg);
    }
    for (uint32_t i = 0; i < np; ++i) {
        pthread_create(&prods[i], NULL, mpmc_prod_thread, &parg);
    }

    for (uint32_t i = 0; i < np; ++i) {
        pthread_join(prods[i], NULL);
    }
    for (uint32_t i = 0; i < nc; ++i) {
        pthread_join(cons[i], NULL);
    }

    uint64_t t1 = get_time_ns();
    double elapsed = (double)(t1 - t0) / 1e9;

    rbipc_detach(ring);

    out_res->num_prod = np;
    out_res->num_cons = nc;
    out_res->msgs_per_sec = (double)total / elapsed;
    out_res->avg_latency_ns = (elapsed * 1e9) / (double)total;

    printf("  MPMC (%uP - %uC): %10.2f msgs/sec | %7.2f ns/msg\n",
           np, nc, out_res->msgs_per_sec, out_res->avg_latency_ns);
}
