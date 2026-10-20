/**
 * @file suite_payload.c
 * @brief Variable payload size throughput scaling benchmark
 */

#include "suite_common.h"

void run_bench_payload(uint32_t payload_size, int count, payload_result_t *out_res) {
    rbipc_destroy(BENCH_SHM_A);

    rbipc_ring_t *ring = NULL;
    assert(rbipc_create(BENCH_SHM_A, 512, payload_size, &ring) == RBIPC_OK);

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(1);
    }

    if (pid == 0) {
        rbipc_ring_t *p_ring = NULL;
        assert(rbipc_attach(BENCH_SHM_A, &p_ring) == RBIPC_OK);

        for (int i = 0; i < count; ++i) {
            void *buf = NULL;
            uint64_t ticket = 0;
            assert(rbipc_reserve_write(p_ring, payload_size, &buf, &ticket) == RBIPC_OK);
            *(uint32_t *)buf = (uint32_t)i;
            assert(rbipc_commit_write(p_ring, ticket, payload_size) == RBIPC_OK);
        }

        rbipc_signal_shutdown(p_ring);
        rbipc_detach(p_ring);
        _exit(0);
    }

    uint64_t t0 = get_time_ns();
    int rec = 0;

    while (rec < count) {
        const void *buf = NULL;
        uint32_t len = 0;
        uint64_t ticket = 0;
        int rc = rbipc_read_acquire(ring, &buf, &len, &ticket);
        if (rc == RBIPC_ERR_SHUTDOWN) break;
        assert(rc == RBIPC_OK);
        rec++;
        rbipc_read_release(ring, ticket);
    }

    uint64_t t1 = get_time_ns();
    double elapsed = (double)(t1 - t0) / 1e9;
    waitpid(pid, NULL, 0);

    rbipc_detach(ring);
    rbipc_destroy(BENCH_SHM_A);

    out_res->payload_size = payload_size;
    out_res->msgs_per_sec = (double)rec / elapsed;
    out_res->mb_per_sec = (out_res->msgs_per_sec * (double)payload_size) / (1024.0 * 1024.0);

    printf("  Payload %8u B: %10.2f msgs/sec | %8.2f MB/sec\n",
           payload_size, out_res->msgs_per_sec, out_res->mb_per_sec);
}
