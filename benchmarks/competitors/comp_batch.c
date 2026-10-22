/**
 * @file comp_batch.c
 * @brief Batch enqueue and vector burst scaling benchmark for competitors suite.
 */

#include "comp_common.h"

void run_bench_batch_scaling(FILE *json_fp, uint64_t batch_iters) {
    char hdr_buf[128];
    snprintf(hdr_buf, sizeof(hdr_buf), "BENCH 6: Vector & Burst Batch Scaling -- %lu Ops per Size", (unsigned long)batch_iters);
    comp_print_hdr(hdr_buf);

    int bsizes[] = {1, 4, 8, 16, 32, 64};
    int nb = (int)(sizeof(bsizes)/sizeof(bsizes[0]));

    printf("\n  %s%-12s  %20s  %20s  %20s%s\n",
           COL_BOLD, "Batch Size", "librbipc (Mops/s)", "rte_ring (Mops/s)", "Disruptor (Mops/s)", COL_RESET);
    printf("  %-12s  %20s  %20s  %20s\n",
           "------------", "--------------------", "--------------------", "--------------------");

    if (json_fp) fprintf(json_fp, "  \"batch_scaling\": [\n");

    for (int b = 0; b < nb; b++) {
        uint32_t batch = (uint32_t)bsizes[b];
        uint64_t total = batch_iters, iters = total / batch;
        double rbi_tput = 0.0, rte_tput = 0.0, dis_tput = 0.0;

        /* 1. librbipc Full-Capability Vector Batch (Trusted & Packed, Zero-Spin) */
        rbipc_ring_t *br = NULL;
        rbipc_destroy("/rbench_batch");
        if (rbipc_create_ex("/rbench_batch", RING_CAPACITY, 128,
                            RBIPC_SHM_NAMED | RBIPC_FLAG_TRUSTED | RBIPC_FLAG_PACKED,
                            0660, &br) == RBIPC_OK) {
            rbipc_set_wait_policy(br, RBIPC_WAIT_ADAPTIVE);
            rbipc_iovec_t iovecs[64];
            rbipc_rovec_t rovecs[64];
            uint64_t tickets[64];
            uint32_t lens[64];
            uint64_t rel_tickets[64];

            uint64_t t0 = comp_get_ns();
            for (uint64_t i = 0; i < iters; i++) {
                uint32_t reserved = 0;
                if (RBIPC_UNLIKELY(rbipc_reserve_write_batch(br, batch, iovecs, &reserved) != RBIPC_OK || reserved == 0))
                    break;
                for (uint32_t k = 0; k < reserved; k++) {
                    tickets[k] = iovecs[k].ticket;
                    lens[k] = 64;
                }
                rbipc_commit_write_batch(br, reserved, tickets, lens);

                uint32_t acquired = 0;
                if (RBIPC_UNLIKELY(rbipc_read_acquire_batch(br, reserved, rovecs, &acquired) != RBIPC_OK || acquired == 0))
                    break;
                for (uint32_t k = 0; k < acquired; k++) {
                    rel_tickets[k] = rovecs[k].ticket;
                }
                rbipc_read_release_batch(br, acquired, rel_tickets);
            }
            uint64_t t1 = comp_get_ns();
            rbi_tput = (double)total / ((double)(t1 - t0) / 1e9);
            rbipc_detach(br);
            rbipc_destroy("/rbench_batch");
        }

        /* 2. DPDK rte_ring Burst Enqueue / Dequeue */
        {
            rte_ring_t *rte_r = rte_ring_alloc(RING_CAPACITY);
            uintptr_t vals[64], out_vals[64];
            for (uint32_t k = 0; k < batch; k++) vals[k] = 0x12345678ULL + k;

            uint64_t t0 = comp_get_ns();
            for (uint64_t i = 0; i < iters; i++) {
                uint32_t enqd = 0, spins = 0;
                while (enqd < batch) {
                    uint32_t ret = rte_mp_enqueue_burst(rte_r, &vals[enqd], batch - enqd);
                    enqd += ret;
                    if (enqd < batch) bench_spin_pause(&spins);
                }

                uint32_t deqd = 0;
                spins = 0;
                while (deqd < batch) {
                    uint32_t ret = rte_mc_dequeue_burst(rte_r, &out_vals[deqd], batch - deqd);
                    deqd += ret;
                    if (deqd < batch) bench_spin_pause(&spins);
                }
            }
            uint64_t t1 = comp_get_ns();
            rte_tput = (double)total / ((double)(t1 - t0) / 1e9);
            rte_ring_free(rte_r);
        }

        /* 3. LMAX Disruptor Batch Claim / Consume */
        {
            disruptor_ring_t *dis_r = disruptor_alloc(RING_CAPACITY);
            uintptr_t vals[64], out_vals[64];
            for (uint32_t k = 0; k < batch; k++) vals[k] = 0x87654321ULL + k;

            uint64_t t0 = comp_get_ns();
            for (uint64_t i = 0; i < iters; i++) {
                disruptor_publish_batch(dis_r, vals, batch);
                uint32_t deqd = 0, spins = 0;
                while (deqd < batch) {
                    uint32_t ret = disruptor_consume_batch(dis_r, -1, &out_vals[deqd], batch - deqd);
                    deqd += ret;
                    if (deqd < batch) bench_spin_pause(&spins);
                }
            }
            uint64_t t1 = comp_get_ns();
            dis_tput = (double)total / ((double)(t1 - t0) / 1e9);
            disruptor_free(dis_r);
        }

        printf("  %-12u  %20.2f  %20.2f  %20.2f\n",
               batch, rbi_tput / 1e6, rte_tput / 1e6, dis_tput / 1e6);

        if (json_fp) {
            fprintf(json_fp, "    {\"batch_size\": %u, \"ops_sec\": %.2f, \"librbipc\": %.2f, \"rte_ring\": %.2f, \"disruptor\": %.2f}%s\n",
                    batch, rbi_tput, rbi_tput, rte_tput, dis_tput, (b == nb - 1 ? "" : ","));
        }
    }

    if (json_fp) {
        fprintf(json_fp, "  ]\n}\n");
        fclose(json_fp);
    }
}
