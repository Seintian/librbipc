/**
 * @file suite_json.c
 * @brief JSON serialization of main benchmark metrics
 */

#include "suite_common.h"

void export_suite_json(const char *tag,
                       const idle_metrics_t *idle,
                       const latency_dist_t *lat,
                       const latency_sla_result_t *sla,
                       const stream_metrics_t *stream,
                       const batch_result_t *batch_results, size_t num_batches,
                       const payload_result_t *payload_results, size_t num_payloads,
                       const mpmc_result_t *mpmc_results, size_t num_mpmc) {
    char json_filename[256];
    char leaf_name[128];
    snprintf(leaf_name, sizeof(leaf_name), "%s.json", tag);
    bench_get_result_path(json_filename, sizeof(json_filename), leaf_name);

    FILE *fp = fopen(json_filename, "w");
    if (!fp) {
        perror("fopen json");
        return;
    }

    bench_hardware_info_t hw;
    bench_get_hardware_info(&hw);

    fprintf(fp, "{\n");
    fprintf(fp, "  \"tag\": \"%s\",\n", tag);
    fprintf(fp, "  \"hardware\": {\n");
    fprintf(fp, "    \"profile\": \"%s\",\n", hw.profile_name);
    fprintf(fp, "    \"cpu\": \"%s\",\n", hw.cpu_model);
    fprintf(fp, "    \"cores\": %d,\n    \"threads\": %d,\n", hw.physical_cores, hw.logical_threads);
    fprintf(fp, "    \"hostname\": \"%s\",\n", hw.hostname);
    fprintf(fp, "    \"kernel\": \"%s\"\n  },\n", hw.kernel);
    fprintf(fp, "  \"idle\": {\n");
    fprintf(fp, "    \"user_sec\": %.6f,\n", idle->user_sec);
    fprintf(fp, "    \"sys_sec\": %.6f,\n", idle->sys_sec);
    fprintf(fp, "    \"total_sec\": %.6f,\n", idle->total_sec);
    fprintf(fp, "    \"cpu_pct\": %.2f,\n", idle->cpu_pct);
    fprintf(fp, "    \"vol_cs\": %ld,\n", idle->voluntary_ctxt_switches);
    fprintf(fp, "    \"invol_cs\": %ld\n", idle->involuntary_ctxt_switches);
    fprintf(fp, "  },\n");
    fprintf(fp, "  \"latency\": {\n");
    fprintf(fp, "    \"min_ns\": %u,\n", lat->min_ns);
    fprintf(fp, "    \"mean_ns\": %.2f,\n", lat->mean_ns);
    fprintf(fp, "    \"p50_ns\": %u,\n", lat->p50_ns);
    fprintf(fp, "    \"p90_ns\": %u,\n", lat->p90_ns);
    fprintf(fp, "    \"p99_ns\": %u,\n", lat->p99_ns);
    fprintf(fp, "    \"p99_9_ns\": %u,\n", lat->p99_9_ns);
    fprintf(fp, "    \"p99_99_ns\": %u,\n", lat->p99_99_ns);
    fprintf(fp, "    \"max_ns\": %u,\n", lat->max_ns);
    fprintf(fp, "    \"stddev_ns\": %.2f\n", lat->stddev_ns);
    fprintf(fp, "  },\n");
    fprintf(fp, "  \"latency_sla\": {\n");
    fprintf(fp, "    \"target_p99_9_us\": %u,\n", sla->threshold_p99_9_us);
    fprintf(fp, "    \"target_max_us\": %u,\n", sla->threshold_max_us);
    fprintf(fp, "    \"observed_p99_9_us\": %u,\n", sla->observed_p99_9_us);
    fprintf(fp, "    \"observed_max_us\": %u,\n", sla->observed_max_us);
    fprintf(fp, "    \"pass\": %s\n", sla->passed ? "true" : "false");
    fprintf(fp, "  },\n");
    fprintf(fp, "  \"streaming\": {\n");
    fprintf(fp, "    \"msgs_per_sec\": %.2f,\n", stream->msgs_per_sec);
    fprintf(fp, "    \"mb_per_sec\": %.2f,\n", stream->mb_per_sec);
    fprintf(fp, "    \"avg_latency_ns\": %.2f,\n", stream->avg_latency_ns);
    fprintf(fp, "    \"producer_vol_cs\": %ld,\n", stream->producer_nvcsw);
    fprintf(fp, "    \"producer_invol_cs\": %ld,\n", stream->producer_nivcsw);
    fprintf(fp, "    \"consumer_vol_cs\": %ld,\n", stream->consumer_nvcsw);
    fprintf(fp, "    \"consumer_invol_cs\": %ld\n", stream->consumer_nivcsw);
    fprintf(fp, "  },\n");
    fprintf(fp, "  \"batching\": [\n");
    for (size_t i = 0; i < num_batches; ++i) {
        fprintf(fp, "    {\"size\": %u, \"msgs_per_sec\": %.2f, \"mb_per_sec\": %.2f, \"avg_latency_ns\": %.2f}%s\n",
                batch_results[i].batch_size, batch_results[i].msgs_per_sec,
                batch_results[i].mb_per_sec, batch_results[i].avg_latency_ns,
                (i + 1 < num_batches) ? "," : "");
    }
    fprintf(fp, "  ],\n");
    fprintf(fp, "  \"payloads\": [\n");
    for (size_t i = 0; i < num_payloads; ++i) {
        fprintf(fp, "    {\"size\": %u, \"msgs_per_sec\": %.2f, \"mb_per_sec\": %.2f}%s\n",
                payload_results[i].payload_size, payload_results[i].msgs_per_sec,
                payload_results[i].mb_per_sec,
                (i + 1 < num_payloads) ? "," : "");
    }
    fprintf(fp, "  ],\n");
    fprintf(fp, "  \"mpmc\": [\n");
    for (size_t i = 0; i < num_mpmc; ++i) {
        fprintf(fp, "    {\"p\": %u, \"c\": %u, \"msgs_per_sec\": %.2f, \"avg_latency_ns\": %.2f}%s\n",
                mpmc_results[i].num_prod, mpmc_results[i].num_cons,
                mpmc_results[i].msgs_per_sec, mpmc_results[i].avg_latency_ns,
                (i + 1 < num_mpmc) ? "," : "");
    }
    fprintf(fp, "  ]\n");
    fprintf(fp, "}\n");
    fclose(fp);

    printf("\nExported JSON benchmark metrics to %s\n", json_filename);
}
