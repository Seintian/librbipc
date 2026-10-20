/**
 * @file idle_json.c
 * @brief Reporting, tabular summary, and JSON telemetry for idle strategies benchmark.
 */

#include "idle_common.h"

void idle_record_res(idle_bench_result_t *results, size_t *n_res,
                     const char *name, const char *cat, double tput,
                     uint64_t *lats, uint64_t *wakes, double idle_cpu, long vol, long invol) {
    qsort(lats, g_lat_iters, sizeof(uint64_t), idle_cmp_u64);
    qsort(wakes, g_wake_iters, sizeof(uint64_t), idle_cmp_u64);
    idle_bench_result_t *r = &results[(*n_res)++];
    strncpy(r->strategy_name, name, sizeof(r->strategy_name) - 1);
    strncpy(r->category, cat, sizeof(r->category) - 1);
    r->tput_mops = tput / 1e6;
    r->tput_mbs  = (tput * (double)MSG_PAYLOAD_SIZE) / (1024.0 * 1024.0);
    r->lat_min   = lats[0];
    r->lat_p50   = idle_pct64(lats, g_lat_iters, 50.0);
    r->lat_p90   = idle_pct64(lats, g_lat_iters, 90.0);
    r->lat_p99   = idle_pct64(lats, g_lat_iters, 99.0);
    r->lat_p999  = idle_pct64(lats, g_lat_iters, 99.9);
    r->lat_max   = lats[g_lat_iters - 1];
    r->wake_min  = wakes[0];
    r->wake_p50  = idle_pct64(wakes, g_wake_iters, 50.0);
    r->wake_p90  = idle_pct64(wakes, g_wake_iters, 90.0);
    r->wake_p99  = idle_pct64(wakes, g_wake_iters, 99.0);
    r->wake_max  = wakes[g_wake_iters - 1];
    r->idle_cpu_pct = idle_cpu;
    r->voluntary_ctx = vol;
    r->involuntary_ctx = invol;

    printf("%s[%-22s]%s Tput: %s%6.2f Mops/s%s (%6.1f MB/s) | Lat p50: %s%5lu ns%s | Wake p50: %s%5lu ns%s | Idle CPU: %s%5.1f%%%s\n",
           COL_BOLD, name, COL_RESET,
           COL_GREEN, r->tput_mops, COL_RESET, r->tput_mbs,
           COL_YELLOW, r->lat_p50, COL_RESET,
           COL_MAGENTA, r->wake_p50, COL_RESET,
           (idle_cpu > 50.0) ? COL_RED : COL_GREEN, idle_cpu, COL_RESET);
    fflush(stdout);
}

void idle_print_table(const idle_bench_result_t *results, size_t n_res) {
    printf("\n%s%s========================================================================================================================%s\n",
           COL_BOLD, COL_CYAN, COL_RESET);
    printf("%s%-22s | %-15s | %10s | %9s | %9s | %9s | %10s | %7s%s\n",
           COL_BOLD, "Strategy Name", "Category", "Throughput", "Lat p50", "Lat p99", "Wake p50", "Wake p99", "Idle CPU", COL_RESET);
    printf("%s-----------------------+-----------------+------------+-----------+-----------+-----------+------------+--------%s\n",
           COL_CYAN, COL_RESET);

    for (size_t i = 0; i < n_res; i++) {
        const idle_bench_result_t *r = &results[i];
        printf("%-22s | %-15s | %6.2f Mop/s | %6lu ns | %6lu ns | %6lu ns | %7lu ns | %5.1f%%\n",
               r->strategy_name, r->category, r->tput_mops, r->lat_p50, r->lat_p99, r->wake_p50, r->wake_p99, r->idle_cpu_pct);
    }
    printf("%s========================================================================================================================%s\n\n",
           COL_CYAN, COL_RESET);
    fflush(stdout);
}

void idle_export_json(const char *json_path, const bench_hardware_info_t *hw,
                      const idle_bench_result_t *results, size_t n_res) {
    FILE *fp = fopen(json_path, "w");
    if (!fp) {
        fprintf(stderr, "Failed to open %s for JSON export: %s\n", json_path, strerror(errno));
        return;
    }

    fprintf(fp, "{\n");
    fprintf(fp, "  \"timestamp\": %lu,\n", idle_get_ns());
    fprintf(fp, "  \"hardware\": {\n");
    fprintf(fp, "    \"profile\": \"%s\",\n", hw->profile_name);
    fprintf(fp, "    \"cpu\": \"%s\",\n", hw->cpu_model);
    fprintf(fp, "    \"cores\": %d,\n", hw->physical_cores);
    fprintf(fp, "    \"threads\": %d,\n", hw->logical_threads);
    fprintf(fp, "    \"hostname\": \"%s\",\n", hw->hostname);
    fprintf(fp, "    \"kernel\": \"%s\"\n", hw->kernel);
    fprintf(fp, "  },\n");
    fprintf(fp, "  \"results\": [\n");
    for (size_t i = 0; i < n_res; i++) {
        const idle_bench_result_t *r = &results[i];
        fprintf(fp, "    {\n");
        fprintf(fp, "      \"strategy\": \"%s\",\n", r->strategy_name);
        fprintf(fp, "      \"category\": \"%s\",\n", r->category);
        fprintf(fp, "      \"throughput_mops\": %.4f,\n", r->tput_mops);
        fprintf(fp, "      \"throughput_mbs\": %.2f,\n", r->tput_mbs);
        fprintf(fp, "      \"lat_p50_ns\": %lu,\n", r->lat_p50);
        fprintf(fp, "      \"lat_p90_ns\": %lu,\n", r->lat_p90);
        fprintf(fp, "      \"lat_p99_ns\": %lu,\n", r->lat_p99);
        fprintf(fp, "      \"lat_p999_ns\": %lu,\n", r->lat_p999);
        fprintf(fp, "      \"wake_p50_ns\": %lu,\n", r->wake_p50);
        fprintf(fp, "      \"wake_p90_ns\": %lu,\n", r->wake_p90);
        fprintf(fp, "      \"wake_p99_ns\": %lu,\n", r->wake_p99);
        fprintf(fp, "      \"wake_max_ns\": %lu,\n", r->wake_max);
        fprintf(fp, "      \"idle_cpu_pct\": %.2f,\n", r->idle_cpu_pct);
        fprintf(fp, "      \"voluntary_ctx_switches\": %ld,\n", r->voluntary_ctx);
        fprintf(fp, "      \"involuntary_ctx_switches\": %ld\n", r->involuntary_ctx);
        fprintf(fp, "    }%s\n", (i + 1 < n_res) ? "," : "");
    }
    fprintf(fp, "  ]\n");
    fprintf(fp, "}\n");
    fclose(fp);
    printf("%s[INFO]%s JSON output saved to %s\n",
           COL_GREEN, COL_RESET, json_path);
}
