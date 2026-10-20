/**
 * @file deep_json.c
 * @brief JSON serialization of deep dive micro-benchmark metrics
 */

#include "deep_common.h"

void export_deep_dive_json(const backoff_bench_result_t *zero_spin,
                           const backoff_bench_result_t *adaptive,
                           const stream_store_bench_t *stream_res, size_t num_sp,
                           const pubsub_fanout_bench_t *pubsub_res, size_t num_subs,
                           const reactor_bench_result_t *reactor_res,
                           const simd_scan_bench_t *simd_res,
                           const lease_bench_result_t *lease_res) {
    char json_path[256];
    bench_get_result_path(json_path, sizeof(json_path), "deep_dive_features.json");
    FILE *fp = fopen(json_path, "w");
    if (!fp) {
        perror("fopen json");
        return;
    }

    fprintf(fp, "{\n");
    fprintf(fp, "  \"adaptive_vs_zerospin\": {\n");
    fprintf(fp, "    \"zerospin\": {\"msgs_per_sec\": %.2f, \"p50_ns\": %u, \"p90_ns\": %u, \"p99_ns\": %u, \"max_ns\": %u},\n",
            zero_spin->msgs_per_sec, zero_spin->p50_ns, zero_spin->p90_ns, zero_spin->p99_ns, zero_spin->max_ns);
    fprintf(fp, "    \"adaptive\": {\"msgs_per_sec\": %.2f, \"p50_ns\": %u, \"p90_ns\": %u, \"p99_ns\": %u, \"max_ns\": %u}\n",
            adaptive->msgs_per_sec, adaptive->p50_ns, adaptive->p90_ns, adaptive->p99_ns, adaptive->max_ns);
    fprintf(fp, "  },\n");
    fprintf(fp, "  \"streaming_stores\": [\n");
    for (size_t i = 0; i < num_sp; ++i) {
        fprintf(fp, "    {\"size\": %u, \"std_mb_s\": %.2f, \"stream_mb_s\": %.2f, \"speedup_pct\": %.2f}%s\n",
                stream_res[i].payload_size, stream_res[i].std_mb_per_sec,
                stream_res[i].stream_mb_per_sec, stream_res[i].speedup_pct,
                (i + 1 < num_sp) ? "," : "");
    }
    fprintf(fp, "  ],\n");
    fprintf(fp, "  \"pubsub\": [\n");
    for (size_t i = 0; i < num_subs; ++i) {
        fprintf(fp, "    {\"subs\": %u, \"publish_msgs_s\": %.2f, \"publish_mb_s\": %.2f, \"delivered_msgs_s\": %.2f}%s\n",
                pubsub_res[i].num_subscribers, pubsub_res[i].msgs_per_sec,
                pubsub_res[i].mb_per_sec, pubsub_res[i].effective_delivery_rate,
                (i + 1 < num_subs) ? "," : "");
    }
    fprintf(fp, "  ],\n");
    fprintf(fp, "  \"reactor\": {\"events_per_sec\": %.2f, \"avg_latency_ns\": %.2f},\n",
            reactor_res->events_per_sec, reactor_res->avg_latency_ns);
    fprintf(fp, "  \"simd_scanning\": {\"scalar_ns\": %.2f, \"simd_ns\": %.2f, \"speedup_pct\": %.2f},\n",
            simd_res->scalar_ns_per_batch, simd_res->simd_ns_per_batch, simd_res->speedup_pct);
    fprintf(fp, "  \"lease_overhead\": {\"no_lease_msgs_s\": %.2f, \"with_lease_msgs_s\": %.2f, \"overhead_pct\": %.2f}\n",
            lease_res->no_lease_msgs_per_sec, lease_res->with_lease_msgs_per_sec, lease_res->overhead_pct);
    fprintf(fp, "}\n");
    fclose(fp);

    printf("\nExported deep dive metrics to %s\n", json_path);
}
