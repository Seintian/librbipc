/**
 * @file bench_deep_dive.c
 * @brief Comparative micro-benchmark coordinator for librbipc architectural features
 */

#include "deep_dive/deep_common.h"

int main(void) {
    printf("================================================================================\n");
    printf("     LIBRBIPC PHASE 1-5 DEEP DIVE ARCHITECTURAL BENCHMARK SUITE\n");
    printf("================================================================================\n");

    printf("\n--> [Benchmark 1/4] Adaptive Backoff (RBIPC_WAIT_ADAPTIVE) vs Zero-Spin (RBIPC_WAIT_ZERO_SPIN)\n");
    backoff_bench_result_t zero_spin_res;
    run_backoff_comparison(RBIPC_WAIT_ZERO_SPIN, &zero_spin_res);
    printf("    RBIPC_WAIT_ZERO_SPIN : %10.2f msgs/s | p50: %5u ns | p90: %5u ns | p99: %5u ns | max: %u ns\n",
           zero_spin_res.msgs_per_sec, zero_spin_res.p50_ns, zero_spin_res.p90_ns, zero_spin_res.p99_ns, zero_spin_res.max_ns);

    backoff_bench_result_t adaptive_res;
    run_backoff_comparison(RBIPC_WAIT_ADAPTIVE, &adaptive_res);
    printf("    RBIPC_WAIT_ADAPTIVE  : %10.2f msgs/s | p50: %5u ns | p90: %5u ns | p99: %5u ns | max: %u ns\n",
           adaptive_res.msgs_per_sec, adaptive_res.p50_ns, adaptive_res.p90_ns, adaptive_res.p99_ns, adaptive_res.max_ns);

    printf("\n--> [Benchmark 2/4] Non-Temporal Streaming Stores (rbipc_write_stream) vs Standard Cached memcpy\n");
    uint32_t stream_payloads[] = { 65536, 262144, 1048576, 4194304 };
    size_t num_sp = sizeof(stream_payloads) / sizeof(stream_payloads[0]);
    stream_store_bench_t stream_results[4];
    for (size_t i = 0; i < num_sp; ++i) {
        run_stream_store_bench(stream_payloads[i], 1000, &stream_results[i]);
        printf("    Payload %7u B: Std Cached: %8.2f MB/s | Non-Temporal: %8.2f MB/s | Delta: %+6.2f%%\n",
               stream_results[i].payload_size, stream_results[i].std_mb_per_sec,
               stream_results[i].stream_mb_per_sec, stream_results[i].speedup_pct);
    }

    printf("\n--> [Benchmark 3/4] 1-to-N Multicast PubSub Engine Fanout (50,000 published msgs)\n");
    uint32_t fanouts[] = { 1, 2, 4 };
    pubsub_fanout_bench_t pubsub_results[3];
    for (size_t i = 0; i < 3; ++i) {
        run_pubsub_fanout_bench(fanouts[i], 50000, &pubsub_results[i]);
        printf("    Fanout 1 -> %u Subs : Publish: %10.2f msgs/s (%6.2f MB/s) | Effective Delivered: %10.2f msgs/s\n",
               pubsub_results[i].num_subscribers, pubsub_results[i].msgs_per_sec,
               pubsub_results[i].mb_per_sec, pubsub_results[i].effective_delivery_rate);
    }

    printf("\n--> [Benchmark 4/4] Eventfd Reactor Asynchronous epoll Notification Latency (50,000 events)\n");
    reactor_bench_result_t reactor_res;
    run_reactor_bench(50000, &reactor_res);
    printf("    Reactor Throughput   : %10.2f events/s | Average Wake Latency: %.2f ns\n",
           reactor_res.events_per_sec, reactor_res.avg_latency_ns);

    printf("\n--> [Benchmark 5/6] SIMD (AVX2) Sequence Scanning vs Scalar Iteration (Batch Size 32, 500k scans)\n");
    simd_scan_bench_t simd_res;
    run_simd_scan_bench(500000, &simd_res);
    printf("    Scalar Loop Scan     : %.2f ns/batch\n", simd_res.scalar_ns_per_batch);
    printf("    AVX2 Vectorized Scan : %.2f ns/batch | Speedup: %+6.2f%%\n",
           simd_res.simd_ns_per_batch, simd_res.speedup_pct);

    printf("\n--> [Benchmark 6/6] Consumer Lease Tracking Hot-Path Overhead (500,000 round-trips)\n");
    lease_bench_result_t lease_res;
    run_lease_overhead_bench(500000, &lease_res);
    printf("    Without Lease Check  : %10.2f msgs/s\n", lease_res.no_lease_msgs_per_sec);
    printf("    With 50ms Lease SLA  : %10.2f msgs/s | Net Overhead: %5.2f%%\n",
           lease_res.with_lease_msgs_per_sec, lease_res.overhead_pct);

    export_deep_dive_json(&zero_spin_res, &adaptive_res,
                          stream_results, num_sp,
                          pubsub_results, 3,
                          &reactor_res, &simd_res, &lease_res);

    printf("================================================================================\n");
    printf("     DEEP DIVE BENCHMARKS COMPLETE\n");
    printf("================================================================================\n");
    return 0;
}
