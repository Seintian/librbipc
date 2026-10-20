/**
 * @file deep_stream.c
 * @brief Non-temporal streaming stores vs standard cached writes micro-benchmark
 */

#include "deep_common.h"

void run_stream_store_bench(uint32_t payload_size, int iterations, stream_store_bench_t *out) {
    rbipc_destroy(DEEP_SHM_A);
    rbipc_ring_t *ring = NULL;
    assert(rbipc_create(DEEP_SHM_A, 64, payload_size, &ring) == RBIPC_OK);

    void *src_buffer = malloc(payload_size);
    memset(src_buffer, 0xAB, payload_size);

    /* Test 1: Standard write (memcpy) */
    uint64_t t0 = get_time_ns();
    for (int i = 0; i < iterations; ++i) {
        void *wbuf = NULL;
        uint64_t wticket = 0;
        assert(rbipc_reserve_write(ring, payload_size, &wbuf, &wticket) == RBIPC_OK);
        memcpy(wbuf, src_buffer, payload_size);
        assert(rbipc_commit_write(ring, wticket, payload_size) == RBIPC_OK);

        const void *rbuf = NULL;
        uint32_t rlen = 0;
        uint64_t rticket = 0;
        assert(rbipc_read_acquire(ring, &rbuf, &rlen, &rticket) == RBIPC_OK);
        rbipc_read_release(ring, rticket);
    }
    uint64_t t1 = get_time_ns();

    double sec_std = (double)(t1 - t0) / (double)RBIPC_NSEC_PER_SEC;
    double mb_std = ((double)iterations * payload_size) / (1024.0 * 1024.0 * sec_std);

    /* Test 2: Non-temporal streaming write */
    uint64_t t2 = get_time_ns();
    for (int i = 0; i < iterations; ++i) {
        uint64_t stream_ticket = 0;
        int rc = rbipc_write_stream(ring, src_buffer, payload_size, &stream_ticket);
        assert(rc == RBIPC_OK);

        const void *rbuf = NULL;
        uint32_t rlen = 0;
        uint64_t rticket = 0;
        assert(rbipc_read_acquire(ring, &rbuf, &rlen, &rticket) == RBIPC_OK);
        rbipc_read_release(ring, rticket);
    }
    uint64_t t3 = get_time_ns();

    double sec_stream = (double)(t3 - t2) / (double)RBIPC_NSEC_PER_SEC;
    double mb_stream = ((double)iterations * payload_size) / (1024.0 * 1024.0 * sec_stream);

    out->payload_size      = payload_size;
    out->std_mb_per_sec    = mb_std;
    out->stream_mb_per_sec = mb_stream;
    out->speedup_pct       = ((mb_stream - mb_std) / mb_std) * 100.0;

    free(src_buffer);
    rbipc_detach(ring);
    rbipc_destroy(DEEP_SHM_A);
}
