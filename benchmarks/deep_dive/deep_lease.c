/**
 * @file deep_lease.c
 * @brief Consumer lease tracking hot-path overhead micro-benchmark
 */

#include "deep_common.h"

void run_lease_overhead_bench(int iterations, lease_bench_result_t *out) {
    rbipc_destroy(DEEP_SHM_A);
    rbipc_ring_t *ring = NULL;
    assert(rbipc_create(DEEP_SHM_A, 1024, 64, &ring) == RBIPC_OK);

    /* 1. Without Lease (default lease_timeout_ns == 0) */
    uint64_t t0 = get_time_ns();
    for (int i = 0; i < iterations; ++i) {
        void *wbuf = NULL;
        uint64_t wticket = 0;
        assert(rbipc_reserve_write(ring, 32, &wbuf, &wticket) == RBIPC_OK);
        assert(rbipc_commit_write(ring, wticket, 32) == RBIPC_OK);

        const void *rbuf = NULL;
        uint32_t rlen = 0;
        uint64_t rticket = 0;
        assert(rbipc_read_acquire(ring, &rbuf, &rlen, &rticket) == RBIPC_OK);
        rbipc_read_release(ring, rticket);
    }
    uint64_t t1 = get_time_ns();

    /* 2. With Lease enabled (e.g. 50ms deadline) */
    assert(rbipc_set_lease_timeout(ring, 50000000ULL) == RBIPC_OK);
    uint64_t t2 = get_time_ns();
    for (int i = 0; i < iterations; ++i) {
        void *wbuf = NULL;
        uint64_t wticket = 0;
        assert(rbipc_reserve_write(ring, 32, &wbuf, &wticket) == RBIPC_OK);
        assert(rbipc_commit_write(ring, wticket, 32) == RBIPC_OK);

        const void *rbuf = NULL;
        uint32_t rlen = 0;
        uint64_t rticket = 0;
        assert(rbipc_read_acquire(ring, &rbuf, &rlen, &rticket) == RBIPC_OK);
        rbipc_read_release(ring, rticket);
    }
    uint64_t t3 = get_time_ns();

    rbipc_detach(ring);
    rbipc_destroy(DEEP_SHM_A);

    double sec_no = (double)(t1 - t0) / (double)RBIPC_NSEC_PER_SEC;
    double sec_with = (double)(t3 - t2) / (double)RBIPC_NSEC_PER_SEC;

    out->no_lease_msgs_per_sec   = (double)iterations / sec_no;
    out->with_lease_msgs_per_sec = (double)iterations / sec_with;
    out->overhead_pct            = ((sec_with - sec_no) / sec_no) * 100.0;
}
