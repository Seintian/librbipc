/**
 * @file opt_streaming.c
 * @brief Module 2: Streaming 1P-1C Throughput & Cache Saturation (Pinned vs Unpinned).
 */

#include "arch_common.h"

void run_module2_streaming(uint64_t iterations, m2_streaming_result_t results[8]) {
    arch_print_subhdr("Module 2: Streaming 1P-1C Throughput & Cache Saturation");
    printf("Messages: %lu (Payload: 64 bytes) | Evaluated: Unpinned vs Pinned (CPU 2 -> CPU 3)\n\n",
           (unsigned long)iterations);

    const struct {
        const char *name;
        uint32_t flags;
    } modes[4] = {
        { "Tier 1 Zero-Trust", 0 },
        { "Tier 2 Trusted",    RBIPC_FLAG_TRUSTED },
        { "Tier 3 Packed",     RBIPC_FLAG_PACKED },
        { "Tier 2+3 Combined", RBIPC_FLAG_TRUSTED | RBIPC_FLAG_PACKED }
    };

    int idx = 0;
    for (int m = 0; m < 4; ++m) {
        for (int p = 0; p < 2; ++p) {
            bool pinned = (p == 1);
            results[idx].mode_name = modes[m].name;
            results[idx].flags = modes[m].flags;
            results[idx].pinned = pinned;

            rbipc_destroy(ARCH_SHM_STREAM);
            rbipc_ring_t *ring = NULL;
            assert(rbipc_create_ex(ARCH_SHM_STREAM, 1024, 256,
                                  RBIPC_SHM_NAMED | modes[m].flags, 0660, &ring) == RBIPC_OK);

            fflush(stdout);
            pid_t pid = fork();
            assert(pid >= 0);

            if (pid == 0) {
                if (pinned) arch_pin_to_core(2); else arch_unpin_core();
                rbipc_ring_t *p_ring = NULL;
                assert(rbipc_attach(ARCH_SHM_STREAM, &p_ring) == RBIPC_OK);

                for (uint64_t i = 0; i < iterations; ++i) {
                    void *wbuf = NULL;
                    uint64_t wticket = 0;
                    int rc = rbipc_reserve_write(p_ring, 64, &wbuf, &wticket);
                    if (rc != RBIPC_OK) break;
                    *(uint64_t *)wbuf = i;
                    rc = rbipc_commit_write(p_ring, wticket, 64);
                    if (rc != RBIPC_OK) break;
                }

                rbipc_signal_shutdown(p_ring);
                rbipc_detach(p_ring);
                _exit(0);
            }

            if (pinned) arch_pin_to_core(3); else arch_unpin_core();
            rbipc_ring_t *c_ring = NULL;
            assert(rbipc_attach(ARCH_SHM_STREAM, &c_ring) == RBIPC_OK);

            uint64_t t0 = arch_get_time_ns();
            uint64_t received = 0;
            while (received < iterations) {
                const void *rbuf = NULL;
                uint32_t rlen = 0;
                uint64_t rticket = 0;
                int rc = rbipc_read_acquire(c_ring, &rbuf, &rlen, &rticket);
                if (rc == RBIPC_OK) {
                    received++;
                    rbipc_read_release(c_ring, rticket);
                } else if (rc == RBIPC_ERR_SHUTDOWN) {
                    break;
                }
            }
            uint64_t t1 = arch_get_time_ns();

            waitpid(pid, NULL, 0);
            arch_unpin_core();

            rbipc_detach(c_ring);
            rbipc_detach(ring);
            rbipc_destroy(ARCH_SHM_STREAM);

            double elapsed_sec = (double)(t1 - t0) / 1e9;
            results[idx].msgs_per_sec = (double)received / elapsed_sec;
            results[idx].mb_per_sec = (results[idx].msgs_per_sec * 64.0) / (1024.0 * 1024.0);
            results[idx].avg_latency_ns = (elapsed_sec * 1e9) / (double)received;

            printf("  %-20s [%-8s] : %10.2f msgs/s | %7.2f MB/s | %6.2f ns/msg\n",
                   results[idx].mode_name, pinned ? "Pinned" : "Unpinned",
                   results[idx].msgs_per_sec, results[idx].mb_per_sec,
                   results[idx].avg_latency_ns);
            idx++;
        }
    }
    printf("\n");
}
