/**
 * @file bench_suite.c
 * @brief Benchmark suite coordinator for librbipc
 */

#include "suite/suite_common.h"

int main(int argc, char **argv) {
    bool strict_regression = false;
    uint32_t custom_p99_9_us = 0;
    uint32_t custom_max_us = 0;

    const char *strict_env = getenv("RBIPC_BENCH_STRICT_REGRESSION");
    if (strict_env && (strcmp(strict_env, "1") == 0 || strcmp(strict_env, "true") == 0)) {
        strict_regression = true;
    }

    const char *tag = "zero_spin_optimized";
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--strict") == 0 || strcmp(argv[i], "--strict-latency") == 0) {
            strict_regression = true;
        } else if (strncmp(argv[i], "--p99-threshold=", 16) == 0) {
            custom_p99_9_us = (uint32_t)strtoul(argv[i] + 16, NULL, 10);
        } else if (strncmp(argv[i], "--max-threshold=", 16) == 0) {
            custom_max_us = (uint32_t)strtoul(argv[i] + 16, NULL, 10);
        } else if (argv[i][0] != '-') {
            tag = argv[i];
        }
    }

    printf("================================================================================\n");
    printf("            LIBRBIPC EXHAUSTIVE BENCHMARK SUITE: [%s]\n", tag);
    if (strict_regression) {
        printf("            [STRICT REGRESSION ENFORCEMENT ACTIVE]\n");
    }
    printf("================================================================================\n");

    idle_metrics_t idle;
    run_bench_idle(&idle);

    int ping_pong_iters = (int)bench_scale_iterations(100000, 20000);
    latency_dist_t lat;
    run_bench_ping_pong(ping_pong_iters, &lat);

    latency_sla_result_t sla_res = {0};
    bool sla_ok = verify_latency_regression(&lat, strict_regression, custom_p99_9_us, custom_max_us, &sla_res);
    if (!sla_ok && strict_regression) {
        fprintf(stderr, "\nFATAL: Aborting benchmark suite due to latency regression failure in strict mode.\n");
        return 1;
    }

    int stream_iters = (int)bench_scale_iterations(1000000, 200000);
    stream_metrics_t stream;
    run_bench_streaming(stream_iters, &stream);

    int batch_iters = (int)bench_scale_iterations(1000000, 200000);
    printf("\n[4/6] Running B-Queue Vector Batching Scaling (%d msgs each)...\n", batch_iters);
    uint32_t batch_sizes[] = { 1, 4, 8, 16, 32, 64, 128 };
    size_t num_batches = sizeof(batch_sizes) / sizeof(batch_sizes[0]);
    batch_result_t batch_results[num_batches];
    for (size_t i = 0; i < num_batches; ++i) {
        run_bench_batching(batch_sizes[i], batch_iters, &batch_results[i]);
    }

    printf("\n[5/6] Running Payload Size Scaling...\n");
    uint32_t payloads[] = { 64, 256, 1024, 4096, 16384, 65536, 262144, 1048576, 4194304, 16777216 };
    size_t num_payloads = sizeof(payloads) / sizeof(payloads[0]);
    payload_result_t payload_results[num_payloads];
    for (size_t i = 0; i < num_payloads; ++i) {
        int payload_iters = (int)bench_scale_iterations(200000, 50000);
        if (payloads[i] >= 1048576) {
            payload_iters = (int)bench_scale_iterations(10000, 2000);
        } else if (payloads[i] >= 65536) {
            payload_iters = (int)bench_scale_iterations(50000, 10000);
        }
        run_bench_payload(payloads[i], payload_iters, &payload_results[i]);
    }

    int mpmc_iters = (int)bench_scale_iterations(500000, 100000);
    printf("\n[6/6] Running MPMC Concurrent Contention Scalability (%d msgs each)...\n", mpmc_iters);
    mpmc_result_t mpmc_results[4];
    run_bench_mpmc(1, 1, mpmc_iters, &mpmc_results[0]);
    run_bench_mpmc(2, 2, mpmc_iters, &mpmc_results[1]);
    run_bench_mpmc(4, 2, mpmc_iters, &mpmc_results[2]);
    run_bench_mpmc(4, 4, mpmc_iters, &mpmc_results[3]);

    export_suite_json(tag, &idle, &lat, &sla_res, &stream,
                      batch_results, num_batches,
                      payload_results, num_payloads,
                      mpmc_results, 4);

    printf("================================================================================\n");
    printf("            BENCHMARK COMPLETE: [%s]\n", tag);
    printf("================================================================================\n");
    return 0;
}
