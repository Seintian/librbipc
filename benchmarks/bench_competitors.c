/**
 * @file bench_competitors.c
 * @brief Master coordinator for cross-system IPC benchmarks (librbipc vs. DPDK, Disruptor, mqueue, AF_UNIX).
 * @details Compiles with atomic modules under benchmarks/competitors/.
 */

#include "competitors/comp_common.h"

static const struct { size_t sz; const char *label; } psizes[] = {
    {64,    "  64 B"}, {256,   " 256 B"}, {1024,  "  1 KB"},
    {4096,  "  4 KB"}, {16384, " 16 KB"}, {65536, " 64 KB"},
};
#define NPSIZES ((int)(sizeof(psizes)/sizeof(psizes[0])))

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    /* Clean up any leftover artifacts */
    rbipc_destroy("/rbench_rbipc_tput");
    rbipc_destroy("/rbench_lat_a");
    rbipc_destroy("/rbench_lat_b");
    rbipc_destroy("/rbench_batch");
    rbipc_destroy("/rbench_mpmc");
    rbipc_destroy("/rbench_idle");
    mq_unlink(MQ_NAME_AB); mq_unlink(MQ_NAME_BA);

    printf("\n");
    printf("%s%s=== librbipc vs. DPDK rte_ring | LMAX Disruptor | POSIX mqueue | AF_UNIX DGRAM ===%s\n",
           COL_BOLD, COL_CYAN, COL_RESET);
    printf("%s%s    Cross-System Competing-Consumer IPC Benchmark%s\n", COL_BOLD, COL_CYAN, COL_RESET);
    printf("%s%s    Evaluation Targets: DPDK, Linux Kernel, Disruptor-cpp, librbipc%s\n", COL_BOLD, COL_CYAN, COL_RESET);
    printf("%s%s    librbipc Mode: Tier 2 Trusted Mode (pure Vyukov turn sequence parity with DPDK/Disruptor)%s\n\n",
           COL_BOLD, COL_GREEN, COL_RESET);

    char json_path[256];
    bench_get_result_path(json_path, sizeof(json_path), "competitors_benchmark.json");
    FILE *json_fp = fopen(json_path, "w");
    if (json_fp) {
        bench_hardware_info_t hw;
        bench_get_hardware_info(&hw);
        fprintf(json_fp, "{\n  \"timestamp\": %lu,\n", (unsigned long)time(NULL));
        fprintf(json_fp, "  \"hardware\": {\n");
        fprintf(json_fp, "    \"profile\": \"%s\",\n", hw.profile_name);
        fprintf(json_fp, "    \"cpu\": \"%s\",\n", hw.cpu_model);
        fprintf(json_fp, "    \"cores\": %d,\n    \"threads\": %d,\n", hw.physical_cores, hw.logical_threads);
        fprintf(json_fp, "    \"hostname\": \"%s\",\n", hw.hostname);
        fprintf(json_fp, "    \"kernel\": \"%s\"\n  },\n", hw.kernel);
    }

    uint64_t bench_iters = bench_scale_iterations(BENCH_ITERATIONS, 100000ULL);
    uint64_t lat_iters = bench_scale_iterations(LAT_ITERATIONS, 15000ULL);
    uint64_t payload_iters = bench_scale_iterations(50000ULL, 20000ULL);
    uint64_t mpmc_iters = bench_scale_iterations(100000ULL, 25000ULL);
    uint64_t batch_iters = bench_scale_iterations(200000ULL, 100000ULL);

    /* BENCH 1: 1P-1C Throughput (64 B) */
    char hdr_buf[128];
    snprintf(hdr_buf, sizeof(hdr_buf), "BENCH 1: 1P-1C Throughput -- 64 B Payloads, %lu Iterations", (unsigned long)bench_iters);
    comp_print_hdr(hdr_buf);

    result_t tput[8];
    printf("  [1/8] librbipc (Zero-Trust 64B Copy) ...\n");
    tput[0] = bench_rbipc_tput(64, bench_iters, TRANSFER_VALUE_COPY, 0, "librbipc (Zero-Trust 64B)");
    printf("  [2/8] librbipc (Trusted 64B Copy) ...\n");
    tput[1] = bench_rbipc_tput(64, bench_iters, TRANSFER_VALUE_COPY, RBIPC_FLAG_TRUSTED, "librbipc (Trusted 64B)");
    printf("  [3/8] librbipc (Combined 64B Copy) ...\n");
    tput[2] = bench_rbipc_tput(64, bench_iters, TRANSFER_VALUE_COPY, RBIPC_FLAG_TRUSTED | RBIPC_FLAG_PACKED, "librbipc (Combined 64B)");
    printf("  [4/8] librbipc (Trusted 8B Ptr - Apples-to-Apples) ...\n");
    tput[3] = bench_rbipc_tput(64, bench_iters, TRANSFER_REFERENCE_PASSING, RBIPC_FLAG_TRUSTED | RBIPC_FLAG_PACKED, "librbipc (Trusted 8B Ptr)");
    printf("  [5/8] rte_ring (DPDK pointer passing) ...\n");
    tput[4] = bench_rte_tput(64, bench_iters, TRANSFER_VALUE_COPY);
    printf("  [6/8] LMAX Disruptor (pointer passing) ...\n");
    tput[5] = bench_disruptor_tput(64, bench_iters, TRANSFER_VALUE_COPY);
    printf("  [7/8] POSIX mqueue ...\n");
    tput[6] = bench_mq_tput(64, bench_iters);
    printf("  [8/8] AF_UNIX SOCK_DGRAM ...\n");
    tput[7] = bench_uds_tput(64, bench_iters);

    for (int i = 0; i < 8; i++) { printf("\n"); comp_print_result(&tput[i]); }
    comp_print_table(tput, 8, "1P-1C Streaming Throughput");

    if (json_fp) {
        fprintf(json_fp, "  \"throughput_64b\": [\n");
        for (int i = 0; i < 8; i++) {
            fprintf(json_fp, "    {\"name\": \"%s\", \"ops_sec\": %.2f, \"mb_sec\": %.2f}%s\n",
                    tput[i].name, tput[i].tput_ops, tput[i].tput_mbs, (i == 7 ? "" : ","));
        }
        fprintf(json_fp, "  ],\n");
    }

    /* BENCH 2: Half-RTT Ping-Pong Latency Distribution */
    snprintf(hdr_buf, sizeof(hdr_buf), "BENCH 2: Half-RTT Ping-Pong Latency Distribution -- %lu Iterations", (unsigned long)lat_iters);
    comp_print_hdr(hdr_buf);

    result_t lat[7];
    printf("  [1/7] librbipc (Zero-Trust) latency ...\n");
    lat[0] = bench_rbipc_lat(lat_iters, 0, "librbipc (Zero-Trust)");
    printf("  [2/7] librbipc (Trusted) latency ...\n");
    lat[1] = bench_rbipc_lat(lat_iters, RBIPC_FLAG_TRUSTED, "librbipc (Trusted)");
    printf("  [3/7] librbipc (Combined) latency ...\n");
    lat[2] = bench_rbipc_lat(lat_iters, RBIPC_FLAG_TRUSTED | RBIPC_FLAG_PACKED, "librbipc (Combined)");
    printf("  [4/7] rte_ring latency ...\n");
    lat[3] = bench_rte_lat(lat_iters);
    printf("  [5/7] LMAX Disruptor latency ...\n");
    lat[4] = bench_disruptor_lat(lat_iters);
    printf("  [6/7] POSIX mqueue latency ...\n");
    lat[5] = bench_mq_lat(lat_iters);
    printf("  [7/7] AF_UNIX DGRAM latency ...\n");
    lat[6] = bench_uds_lat(lat_iters);

    for (int i = 0; i < 7; i++) { printf("\n"); comp_print_result(&lat[i]); }
    comp_print_table(lat, 7, "Half-RTT Latency Distribution");

    if (json_fp) {
        fprintf(json_fp, "  \"latency_distribution\": [\n");
        for (int i = 0; i < 7; i++) {
            fprintf(json_fp, "    {\"name\": \"%s\", \"min_ns\": %lu, \"p50_ns\": %lu, \"p90_ns\": %lu, \"p99_ns\": %lu, \"p999_ns\": %lu, \"max_ns\": %lu}%s\n",
                    lat[i].name, (unsigned long)lat[i].lat_min, (unsigned long)lat[i].lat_p50,
                    (unsigned long)lat[i].lat_p90, (unsigned long)lat[i].lat_p99,
                    (unsigned long)lat[i].lat_p999, (unsigned long)lat[i].lat_max,
                    (i == 6 ? "" : ","));
        }
        fprintf(json_fp, "  ],\n");
    }

    /* BENCH 3A: Payload-Size Scaling (Value-Passing Semantics) */
    snprintf(hdr_buf, sizeof(hdr_buf), "BENCH 3A: Payload-Size Scaling (Value-Passing Semantics) -- %lu Iterations per Size", (unsigned long)payload_iters);
    comp_print_hdr(hdr_buf);

    printf("\n  %-8s  %s%16s%s  %16s  %16s  %16s  %16s\n",
           "Size", COL_BOLD, "librbipc", COL_RESET,
           "rte_ring", "Disruptor", "POSIX mqueue", "AF_UNIX DGRAM");
    printf("  %-8s  %16s  %16s  %16s  %16s  %16s\n",
           "--------", "----------------", "----------------", "----------------",
           "----------------", "----------------");

    result_t val_rb[NPSIZES], val_rt[NPSIZES], val_ds[NPSIZES], val_mq[NPSIZES], val_ud[NPSIZES];
    for (int s = 0; s < NPSIZES; s++) {
        size_t sz = psizes[s].sz;
        val_rb[s] = bench_rbipc_tput(sz, payload_iters, TRANSFER_VALUE_COPY, RBIPC_FLAG_TRUSTED, "librbipc");
        val_rt[s] = bench_rte_tput(sz, payload_iters, TRANSFER_VALUE_COPY);
        val_ds[s] = bench_disruptor_tput(sz, payload_iters, TRANSFER_VALUE_COPY);
        val_mq[s] = (sz <= 8192) ? bench_mq_tput(sz, payload_iters) : (result_t){.name="POSIX mqueue", .tput_ops=0};
        val_ud[s] = bench_uds_tput(sz, payload_iters);

        printf("  %-8s  %15.2fM  %15.2fM  %15.2fM  %15.2fM  %15.2fM\n",
               psizes[s].label,
               val_rb[s].tput_ops / 1e6, val_rt[s].tput_ops / 1e6, val_ds[s].tput_ops / 1e6,
               val_mq[s].tput_ops / 1e6, val_ud[s].tput_ops / 1e6);
    }

    /* BENCH 3B: Payload-Size Scaling (Reference-Passing Semantics) */
    snprintf(hdr_buf, sizeof(hdr_buf), "BENCH 3B: Payload-Size Scaling (Reference-Passing Semantics) -- %lu Iterations per Size", (unsigned long)payload_iters);
    comp_print_hdr(hdr_buf);

    printf("\n  %-8s  %s%16s%s  %16s  %16s\n",
           "Size", COL_BOLD, "librbipc", COL_RESET, "rte_ring", "Disruptor");
    printf("  %-8s  %16s  %16s  %16s\n",
           "--------", "----------------", "----------------", "----------------");

    result_t ref_rb[NPSIZES], ref_rt[NPSIZES], ref_ds[NPSIZES];
    for (int s = 0; s < NPSIZES; s++) {
        size_t sz = psizes[s].sz;
        ref_rb[s] = bench_rbipc_tput(sz, payload_iters, TRANSFER_REFERENCE_PASSING, RBIPC_FLAG_TRUSTED | RBIPC_FLAG_PACKED, "librbipc");
        ref_rt[s] = bench_rte_tput(sz, payload_iters, TRANSFER_REFERENCE_PASSING);
        ref_ds[s] = bench_disruptor_tput(sz, payload_iters, TRANSFER_REFERENCE_PASSING);

        printf("  %-8s  %15.2fM  %15.2fM  %15.2fM\n",
               psizes[s].label,
               ref_rb[s].tput_ops / 1e6, ref_rt[s].tput_ops / 1e6, ref_ds[s].tput_ops / 1e6);
    }

    if (json_fp) {
        fprintf(json_fp, "  \"payload_scaling_value\": [\n");
        for (int s = 0; s < NPSIZES; s++) {
            fprintf(json_fp, "    {\"size\": %zu, \"librbipc\": %.2f, \"rte_ring\": %.2f, \"disruptor\": %.2f, \"mqueue\": %.2f, \"af_unix\": %.2f}%s\n",
                    psizes[s].sz, val_rb[s].tput_ops, val_rt[s].tput_ops, val_ds[s].tput_ops, val_mq[s].tput_ops, val_ud[s].tput_ops,
                    (s == NPSIZES - 1 ? "" : ","));
        }
        fprintf(json_fp, "  ],\n");

        fprintf(json_fp, "  \"payload_scaling_reference\": [\n");
        for (int s = 0; s < NPSIZES; s++) {
            fprintf(json_fp, "    {\"size\": %zu, \"librbipc\": %.2f, \"rte_ring\": %.2f, \"disruptor\": %.2f}%s\n",
                    psizes[s].sz, ref_rb[s].tput_ops, ref_rt[s].tput_ops, ref_ds[s].tput_ops,
                    (s == NPSIZES - 1 ? "" : ","));
        }
        fprintf(json_fp, "  ],\n");

        fprintf(json_fp, "  \"payload_scaling\": [\n");
        for (int s = 0; s < NPSIZES; s++) {
            fprintf(json_fp, "    {\"size\": %zu, \"librbipc\": %.2f, \"rte_ring\": %.2f, \"disruptor\": %.2f, \"mqueue\": %.2f, \"af_unix\": %.2f}%s\n",
                    psizes[s].sz, val_rb[s].tput_ops, val_rt[s].tput_ops, val_ds[s].tput_ops, val_mq[s].tput_ops, val_ud[s].tput_ops,
                    (s == NPSIZES - 1 ? "" : ","));
        }
        fprintf(json_fp, "  ],\n");
    }

    /* BENCH 4: Idle CPU */
    run_bench_idle_cpu(json_fp);

    /* BENCH 5: MPMC Scalability */
    run_bench_mpmc(json_fp, mpmc_iters);

    /* BENCH 6: Batch API Scaling */
    run_bench_batch_scaling(json_fp, batch_iters);

    comp_print_sep();
    printf("  Competitors benchmark complete. JSON results exported to %s\n", json_path);
    comp_print_sep();

    return 0;
}
