/**
 * @file opt_batching.c
 * @brief Module 3: B-Queue Vector Batching Scaling.
 */

#include "arch_common.h"

void run_module3_batching(uint64_t iterations_per_batch,
                          m3_batch_result_t results[6]) {
    arch_print_subhdr("Module 3: B-Queue Vector Batching Scaling");
    printf("Messages per Batch Size: %lu | Evaluated Sizes: 1, 4, 16, 32, 64, 128\n\n",
           (unsigned long)iterations_per_batch);

    const uint32_t batch_sizes[6] = { 1, 4, 16, 32, 64, 128 };

    for (int b = 0; b < 6; ++b) {
        uint32_t bsize = batch_sizes[b];
        results[b].batch_size = bsize;

        for (int m = 0; m < 2; ++m) {
            uint32_t flags = (m == 0) ? 0 : (RBIPC_FLAG_TRUSTED | RBIPC_FLAG_PACKED);

            rbipc_destroy(ARCH_SHM_BATCH);
            rbipc_ring_t *ring = NULL;
            assert(rbipc_create_ex(ARCH_SHM_BATCH, 2048, 256,
                                  RBIPC_SHM_NAMED | flags, 0660, &ring) == RBIPC_OK);

            fflush(stdout);
            pid_t pid = fork();
            assert(pid >= 0);

            if (pid == 0) {
                arch_pin_to_core(2);
                rbipc_ring_t *p_ring = NULL;
                assert(rbipc_attach(ARCH_SHM_BATCH, &p_ring) == RBIPC_OK);

                rbipc_iovec_t iovecs[128];
                uint64_t tickets[128];
                uint32_t lens[128];

                uint64_t sent = 0;
                while (sent < iterations_per_batch) {
                    uint32_t req = (uint32_t)(iterations_per_batch - sent);
                    if (req > bsize) req = bsize;

                    uint32_t reserved = 0;
                    int rc = rbipc_reserve_write_batch(p_ring, req, iovecs, &reserved);
                    if (rc != RBIPC_OK) break;

                    for (uint32_t i = 0; i < reserved; ++i) {
                        tickets[i] = iovecs[i].ticket;
                        lens[i] = 64;
                    }

                    rc = rbipc_commit_write_batch(p_ring, reserved, tickets, lens);
                    if (rc != RBIPC_OK) break;

                    sent += reserved;
                }

                rbipc_signal_shutdown(p_ring);
                rbipc_detach(p_ring);
                _exit(0);
            }

            arch_pin_to_core(3);
            rbipc_ring_t *c_ring = NULL;
            assert(rbipc_attach(ARCH_SHM_BATCH, &c_ring) == RBIPC_OK);

            rbipc_rovec_t rovecs[128];
            uint64_t tickets[128];

            uint64_t t0 = arch_get_time_ns();
            uint64_t received = 0;
            while (received < iterations_per_batch) {
                uint32_t req = (uint32_t)(iterations_per_batch - received);
                if (req > bsize) req = bsize;

                uint32_t acquired = 0;
                int rc = rbipc_read_acquire_batch(c_ring, req, rovecs, &acquired);
                if (rc == RBIPC_ERR_SHUTDOWN) break;
                if (rc != RBIPC_OK) break;

                for (uint32_t i = 0; i < acquired; ++i) {
                    tickets[i] = rovecs[i].ticket;
                }

                received += acquired;
                rbipc_read_release_batch(c_ring, acquired, tickets);
            }
            uint64_t t1 = arch_get_time_ns();

            waitpid(pid, NULL, 0);
            arch_unpin_core();

            rbipc_detach(c_ring);
            rbipc_detach(ring);
            rbipc_destroy(ARCH_SHM_BATCH);

            double elapsed_sec = (double)(t1 - t0) / 1e9;
            double msgs_sec = (double)received / elapsed_sec;
            double mb_sec = (msgs_sec * 64.0) / (1024.0 * 1024.0);
            double lat_ns = (elapsed_sec * 1e9) / (double)received;

            if (m == 0) {
                results[b].t1_msgs_sec = msgs_sec;
                results[b].t1_mb_sec = mb_sec;
                results[b].t1_latency_ns = lat_ns;
            } else {
                results[b].t23_msgs_sec = msgs_sec;
                results[b].t23_mb_sec = mb_sec;
                results[b].t23_latency_ns = lat_ns;
            }
        }

        results[b].t1_speedup_vs_b1  = results[b].t1_msgs_sec / results[0].t1_msgs_sec;
        results[b].t23_speedup_vs_b1 = results[b].t23_msgs_sec / results[0].t23_msgs_sec;

        printf("  Batch Size %3u : Tier 1: %10.2f msgs/s (%6.2f MB/s, %5.1f ns) [%4.2fx] | Tier 2+3: %10.2f msgs/s (%6.2f MB/s, %5.1f ns) [%4.2fx]\n",
               bsize, results[b].t1_msgs_sec, results[b].t1_mb_sec, results[b].t1_latency_ns, results[b].t1_speedup_vs_b1,
               results[b].t23_msgs_sec, results[b].t23_mb_sec, results[b].t23_latency_ns, results[b].t23_speedup_vs_b1);
    }
    printf("\n");
}
