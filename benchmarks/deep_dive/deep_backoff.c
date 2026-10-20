/**
 * @file deep_backoff.c
 * @brief Adaptive backoff vs zero-spin passive wait micro-benchmark
 */

#include "deep_common.h"

void run_backoff_comparison(uint32_t policy, backoff_bench_result_t *out_res) {
    rbipc_destroy(DEEP_SHM_A);
    rbipc_destroy(DEEP_SHM_B);

    rbipc_ring_t *ring_a = NULL;
    rbipc_ring_t *ring_b = NULL;
    assert(rbipc_create(DEEP_SHM_A, 256, 64, &ring_a) == RBIPC_OK);
    assert(rbipc_create(DEEP_SHM_B, 256, 64, &ring_b) == RBIPC_OK);

    rbipc_set_wait_policy(ring_a, policy);
    rbipc_set_wait_policy(ring_b, policy);

    int iterations = 50000;

    pid_t pid = fork();
    if (pid == 0) {
        rbipc_ring_t *c_a = NULL, *c_b = NULL;
        assert(rbipc_attach(DEEP_SHM_A, &c_a) == RBIPC_OK);
        assert(rbipc_attach(DEEP_SHM_B, &c_b) == RBIPC_OK);
        rbipc_set_wait_policy(c_a, policy);
        rbipc_set_wait_policy(c_b, policy);

        for (int i = 0; i < iterations; ++i) {
            const void *rbuf = NULL;
            uint32_t rlen = 0;
            uint64_t rticket = 0;
            if (rbipc_read_acquire(c_a, &rbuf, &rlen, &rticket) != RBIPC_OK) break;
            uint64_t ts = *(const uint64_t *)rbuf;
            rbipc_read_release(c_a, rticket);

            void *wbuf = NULL;
            uint64_t wticket = 0;
            assert(rbipc_reserve_write(c_b, sizeof(uint64_t), &wbuf, &wticket) == RBIPC_OK);
            *(uint64_t *)wbuf = ts;
            assert(rbipc_commit_write(c_b, wticket, sizeof(uint64_t)) == RBIPC_OK);
        }
        rbipc_detach(c_a);
        rbipc_detach(c_b);
        _exit(0);
    }

    uint32_t *samples = malloc(sizeof(uint32_t) * (size_t)iterations);
    assert(samples != NULL);

    uint64_t t_start = get_time_ns();
    for (int i = 0; i < iterations; ++i) {
        uint64_t t0 = get_time_ns();
        void *wbuf = NULL;
        uint64_t wticket = 0;
        assert(rbipc_reserve_write(ring_a, sizeof(uint64_t), &wbuf, &wticket) == RBIPC_OK);
        *(uint64_t *)wbuf = t0;
        assert(rbipc_commit_write(ring_a, wticket, sizeof(uint64_t)) == RBIPC_OK);

        const void *rbuf = NULL;
        uint32_t rlen = 0;
        uint64_t rticket = 0;
        assert(rbipc_read_acquire(ring_b, &rbuf, &rlen, &rticket) == RBIPC_OK);
        uint64_t t1 = get_time_ns();
        rbipc_read_release(ring_b, rticket);

        uint64_t rtt = (t1 > t0) ? (t1 - t0) : 1;
        samples[i] = (uint32_t)(rtt / 2);
    }
    uint64_t t_end = get_time_ns();

    waitpid(pid, NULL, 0);
    rbipc_detach(ring_a);
    rbipc_detach(ring_b);
    rbipc_destroy(DEEP_SHM_A);
    rbipc_destroy(DEEP_SHM_B);

    qsort(samples, (size_t)iterations, sizeof(uint32_t), compare_u32);

    double elapsed_sec = (double)(t_end - t_start) / (double)RBIPC_NSEC_PER_SEC;
    out_res->msgs_per_sec = (double)(iterations * 2) / elapsed_sec;
    out_res->p50_ns       = samples[(size_t)(iterations * 0.50)];
    out_res->p90_ns       = samples[(size_t)(iterations * 0.90)];
    out_res->p99_ns       = samples[(size_t)(iterations * 0.99)];
    out_res->max_ns       = samples[iterations - 1];

    free(samples);
}
