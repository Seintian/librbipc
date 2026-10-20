/**
 * @file comp_batch.c
 * @brief Batch enqueue scaling benchmark for competitors suite.
 */

#include "comp_common.h"

void run_bench_batch_scaling(FILE *json_fp, uint64_t batch_iters) {
    char hdr_buf[128];
    snprintf(hdr_buf, sizeof(hdr_buf), "BENCH 6: librbipc Vector/Batch Enqueue Scaling -- %lu Ops", (unsigned long)batch_iters);
    comp_print_hdr(hdr_buf);

    int bsizes[] = {1, 4, 8, 16, 32, 64};
    int nb = (int)(sizeof(bsizes)/sizeof(bsizes[0]));

    printf("\n  %s%-12s  %20s%s\n", COL_BOLD, "Batch Size", "librbipc (Mops/s)", COL_RESET);
    printf("  %-12s  %20s\n", "------------", "--------------------");

    if (json_fp) fprintf(json_fp, "  \"batch_scaling\": [\n");

    for (int b = 0; b < nb; b++) {
        uint32_t batch = (uint32_t)bsizes[b];
        rbipc_ring_t *br = NULL;
        rbipc_destroy("/rbench_batch");
        if (rbipc_create_ex("/rbench_batch", RING_CAPACITY, 128, RBIPC_SHM_NAMED | RBIPC_FLAG_TRUSTED, 0660, &br) == RBIPC_OK) {
            rbipc_set_wait_policy(br, RBIPC_WAIT_ADAPTIVE);
            uint64_t total = batch_iters, iters = total / batch;
            uint64_t t0 = comp_get_ns();
            rbipc_iovec_t iovecs[64];
            uint64_t tickets[64];
            uint32_t lens[64];

            for (uint64_t i = 0; i < iters; i++) {
                uint32_t reserved = 0;
                uint32_t spins = 0;
                while (rbipc_reserve_write_batch(br, batch, iovecs, &reserved) != RBIPC_OK)
                    bench_spin_pause(&spins);
                for (uint32_t k = 0; k < reserved; k++) {
                    tickets[k] = iovecs[k].ticket;
                    lens[k] = 64;
                }
                rbipc_commit_write_batch(br, reserved, tickets, lens);

                for (uint32_t j = 0; j < reserved; j++) {
                    const void *rb; uint64_t rt; uint32_t rl;
                    spins = 0;
                    while (rbipc_read_acquire_nonblock(br, &rb, &rl, &rt) != RBIPC_OK)
                        bench_spin_pause(&spins);
                    rbipc_read_release(br, rt);
                }
            }
            uint64_t t1 = comp_get_ns();
            double tput_val = (double)total / ((double)(t1 - t0) / 1e9);
            printf("  %-12u  %20.2f\n", batch, tput_val / 1e6);

            if (json_fp) {
                fprintf(json_fp, "    {\"batch_size\": %u, \"ops_sec\": %.2f}%s\n",
                        batch, tput_val, (b == nb - 1 ? "" : ","));
            }
            rbipc_detach(br);
            rbipc_destroy("/rbench_batch");
        }
    }
    if (json_fp) {
        fprintf(json_fp, "  ]\n}\n");
        fclose(json_fp);
    }
}
