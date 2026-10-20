/**
 * @file suite_batch.c
 * @brief Vector batching scaling benchmark (B-Queue)
 */

#include "suite_common.h"

void run_bench_batching(uint32_t batch_size, int total_msgs, batch_result_t *out_res) {
    rbipc_destroy(BENCH_SHM_A);

    rbipc_ring_t *ring = NULL;
    assert(rbipc_create(BENCH_SHM_A, 2048, 128, &ring) == RBIPC_OK);

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(1);
    }

    if (pid == 0) {
        /* Producer Child */
        rbipc_ring_t *p_ring = NULL;
        assert(rbipc_attach(BENCH_SHM_A, &p_ring) == RBIPC_OK);

        rbipc_iovec_t iovecs[128];
        uint64_t tickets[128];
        uint32_t lens[128];

        int sent = 0;
        while (sent < total_msgs) {
            uint32_t req = (uint32_t)(total_msgs - sent);
            if (req > batch_size) req = batch_size;

            uint32_t reserved = 0;
            assert(rbipc_reserve_write_batch(p_ring, req, iovecs, &reserved) == RBIPC_OK);

            for (uint32_t i = 0; i < reserved; ++i) {
                *(uint32_t *)iovecs[i].buf = (uint32_t)(sent + i);
                tickets[i] = iovecs[i].ticket;
                lens[i] = 64;
            }

            assert(rbipc_commit_write_batch(p_ring, reserved, tickets, lens) == RBIPC_OK);
            sent += (int)reserved;
        }

        rbipc_signal_shutdown(p_ring);
        rbipc_detach(p_ring);
        _exit(0);
    }

    /* Consumer Parent */
    rbipc_rovec_t rovecs[128];
    uint64_t tickets[128];

    uint64_t t0 = get_time_ns();
    int count = 0;

    while (count < total_msgs) {
        uint32_t req = (uint32_t)(total_msgs - count);
        if (req > batch_size) req = batch_size;

        uint32_t acquired = 0;
        int rc = rbipc_read_acquire_batch(ring, req, rovecs, &acquired);
        if (rc == RBIPC_ERR_SHUTDOWN) break;
        assert(rc == RBIPC_OK);

        for (uint32_t i = 0; i < acquired; ++i) {
            tickets[i] = rovecs[i].ticket;
        }

        assert(rbipc_read_release_batch(ring, acquired, tickets) == RBIPC_OK);
        count += (int)acquired;
    }

    uint64_t t1 = get_time_ns();
    double elapsed = (double)(t1 - t0) / 1e9;
    waitpid(pid, NULL, 0);

    rbipc_detach(ring);
    rbipc_destroy(BENCH_SHM_A);

    out_res->batch_size = batch_size;
    out_res->msgs_per_sec = (double)count / elapsed;
    out_res->mb_per_sec = (out_res->msgs_per_sec * 64.0) / (1024.0 * 1024.0);
    out_res->avg_latency_ns = (elapsed * 1e9) / (double)count;

    printf("  Batch Size %3u: %10.2f msgs/sec | %8.2f MB/sec | %7.2f ns/msg\n",
           batch_size, out_res->msgs_per_sec, out_res->mb_per_sec, out_res->avg_latency_ns);
}
