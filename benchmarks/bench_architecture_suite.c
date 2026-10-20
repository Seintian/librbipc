/**
 * @file bench_architecture_suite.c
 * @brief Master coordinator for the librbipc architectural evaluation benchmark suite.
 * @details Executes all atomic modules in benchmarks/architecture/ and aggregates telemetry.
 */

#include "architecture/arch_common.h"

int main(void) {
    arch_print_banner("librbipc Architectural Evaluation Benchmark Suite");

    bench_hardware_info_t hw;
    bench_get_hardware_info(&hw);
    printf("Hardware Profile: %s%s%s | Host: %s | CPU: %s (%d cores, %d threads)\n",
           COL_BOLD, hw.profile_name, COL_RESET, hw.hostname, hw.cpu_model,
           hw.physical_cores, hw.logical_threads);
    printf("Kernel: %s\n\n", hw.kernel);

    /* Scale iterations according to active hardware profile */
    uint64_t m1_iters = bench_scale_iterations(50000ULL, 50000ULL);
    uint64_t m1_warmup = 500ULL;
    uint64_t m2_iters = bench_scale_iterations(200000ULL, 200000ULL);
    uint64_t m3_iters = bench_scale_iterations(500000ULL, 500000ULL);
    uint64_t m4_cycles = bench_scale_iterations(2000ULL, 2000ULL);
    uint64_t m5_iters = bench_scale_iterations(50000ULL, 50000ULL);
    uint64_t m6_iters = bench_scale_iterations(2500000ULL, 2500000ULL);
    uint64_t m7_fds_iters = bench_scale_iterations(5000ULL, 5000ULL);
    uint64_t m7_futex_iters = bench_scale_iterations(500000ULL, 500000ULL);

    m1_latency_result_t          m1_results[4];
    m2_streaming_result_t        m2_results[8];
    m3_batch_result_t            m3_results[6];
    m4_lifecycle_result_t        m4_results;
    m5_pubsub_result_t           m5_results[16];
    m6_simd_result_t             m6_results[4];
    m7_primitives_result_t       m7_results;
    m8_dynamic_hugepages_result_t m8_results[4];

    run_module1_ping_pong(m1_iters, m1_warmup, m1_results);
    run_module2_streaming(m2_iters, m2_results);
    run_module3_batching(m3_iters, m3_results);
    run_module4_lifecycle(m4_cycles, &m4_results);
    run_module5_pubsub(m5_iters, m5_results);
    run_module6_simd(m6_iters, m6_results);
    run_module7_primitives(m7_fds_iters, m7_futex_iters, &m7_results);
    run_module8_dynamic_hugepages(m2_iters, m8_results);

    char json_path[256];
    bench_get_result_path(json_path, sizeof(json_path), "architecture_benchmark.json");
    export_json_telemetry(json_path, &hw, m1_results, m2_results, m3_results,
                          &m4_results, m5_results, m6_results, &m7_results, m8_results);

    arch_print_banner("Architectural Evaluation Benchmark Suite Verification Complete");
    return 0;
}
