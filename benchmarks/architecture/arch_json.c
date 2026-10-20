/**
 * @file opt_json.c
 * @brief Module 9: JSON Telemetry Generation for the architectural evaluation benchmark suite.
 */

#include "arch_common.h"

void export_json_telemetry(const char *json_path,
                           const bench_hardware_info_t *hw,
                           const m1_latency_result_t m1[4],
                           const m2_streaming_result_t m2[8],
                           const m3_batch_result_t m3[6],
                           const m4_lifecycle_result_t *m4,
                           const m5_pubsub_result_t m5[16],
                           const m6_simd_result_t m6[4],
                           const m7_primitives_result_t *m7,
                           const m8_dynamic_hugepages_result_t m8[4]) {
    FILE *fp = fopen(json_path, "w");
    if (!fp) {
        fprintf(stderr, "Failed to open %s for JSON writing: %s\n", json_path, strerror(errno));
        return;
    }

    fprintf(fp, "{\n");
    fprintf(fp, "  \"timestamp\": %lu,\n", (unsigned long)time(NULL));
    fprintf(fp, "  \"hardware\": {\n");
    fprintf(fp, "    \"profile\": \"%s\",\n", hw->profile_name);
    fprintf(fp, "    \"cpu\": \"%s\",\n", hw->cpu_model);
    fprintf(fp, "    \"cores\": %d,\n", hw->physical_cores);
    fprintf(fp, "    \"threads\": %d,\n", hw->logical_threads);
    fprintf(fp, "    \"hostname\": \"%s\",\n", hw->hostname);
    fprintf(fp, "    \"kernel\": \"%s\"\n", hw->kernel);
    fprintf(fp, "  },\n");

    /* Module 1 */
    fprintf(fp, "  \"module1_latency_hierarchy\": [\n");
    for (int i = 0; i < 4; ++i) {
        fprintf(fp, "    {\n");
        fprintf(fp, "      \"mode\": \"%s\",\n", m1[i].mode_name);
        fprintf(fp, "      \"flags\": %u,\n", m1[i].flags);
        fprintf(fp, "      \"min_ns\": %u,\n", m1[i].min_ns);
        fprintf(fp, "      \"p25_ns\": %u,\n", m1[i].p25_ns);
        fprintf(fp, "      \"p50_ns\": %u,\n", m1[i].p50_ns);
        fprintf(fp, "      \"p75_ns\": %u,\n", m1[i].p75_ns);
        fprintf(fp, "      \"p90_ns\": %u,\n", m1[i].p90_ns);
        fprintf(fp, "      \"p99_ns\": %u,\n", m1[i].p99_ns);
        fprintf(fp, "      \"p99_9_ns\": %u,\n", m1[i].p99_9_ns);
        fprintf(fp, "      \"max_ns\": %u,\n", m1[i].max_ns);
        fprintf(fp, "      \"mean_ns\": %.2f,\n", m1[i].mean_ns);
        fprintf(fp, "      \"reduction_pct\": %.2f\n", m1[i].reduction_pct);
        fprintf(fp, "    }%s\n", (i + 1 < 4) ? "," : "");
    }
    fprintf(fp, "  ],\n");

    /* Module 2 */
    fprintf(fp, "  \"module2_streaming_throughput\": [\n");
    for (int i = 0; i < 8; ++i) {
        fprintf(fp, "    {\n");
        fprintf(fp, "      \"mode\": \"%s\",\n", m2[i].mode_name);
        fprintf(fp, "      \"flags\": %u,\n", m2[i].flags);
        fprintf(fp, "      \"pinned\": %s,\n", m2[i].pinned ? "true" : "false");
        fprintf(fp, "      \"msgs_per_sec\": %.2f,\n", m2[i].msgs_per_sec);
        fprintf(fp, "      \"mb_per_sec\": %.2f,\n", m2[i].mb_per_sec);
        fprintf(fp, "      \"avg_latency_ns\": %.2f\n", m2[i].avg_latency_ns);
        fprintf(fp, "    }%s\n", (i + 1 < 8) ? "," : "");
    }
    fprintf(fp, "  ],\n");

    /* Module 3 */
    fprintf(fp, "  \"module3_batch_scaling\": [\n");
    for (int i = 0; i < 6; ++i) {
        fprintf(fp, "    {\n");
        fprintf(fp, "      \"batch_size\": %u,\n", m3[i].batch_size);
        fprintf(fp, "      \"tier1_msgs_sec\": %.2f,\n", m3[i].t1_msgs_sec);
        fprintf(fp, "      \"tier1_mb_sec\": %.2f,\n", m3[i].t1_mb_sec);
        fprintf(fp, "      \"tier1_latency_ns\": %.2f,\n", m3[i].t1_latency_ns);
        fprintf(fp, "      \"tier1_speedup_vs_b1\": %.2f,\n", m3[i].t1_speedup_vs_b1);
        fprintf(fp, "      \"tier23_msgs_sec\": %.2f,\n", m3[i].t23_msgs_sec);
        fprintf(fp, "      \"tier23_mb_sec\": %.2f,\n", m3[i].t23_mb_sec);
        fprintf(fp, "      \"tier23_latency_ns\": %.2f,\n", m3[i].t23_latency_ns);
        fprintf(fp, "      \"tier23_speedup_vs_b1\": %.2f\n", m3[i].t23_speedup_vs_b1);
        fprintf(fp, "    }%s\n", (i + 1 < 6) ? "," : "");
    }
    fprintf(fp, "  ],\n");

    /* Module 4 */
    fprintf(fp, "  \"module4_shm_lifecycle\": {\n");
    fprintf(fp, "    \"named\": {\"ops_sec\": %.2f, \"latency_us\": %.2f},\n",
            m4->named_ops_sec, m4->named_latency_us);
    fprintf(fp, "    \"anonymous\": {\"ops_sec\": %.2f, \"latency_us\": %.2f},\n",
            m4->anon_ops_sec, m4->anon_latency_us);
    fprintf(fp, "    \"speedup\": %.2f\n", m4->speedup_factor);
    fprintf(fp, "  },\n");

    /* Module 5 */
    fprintf(fp, "  \"module5_pubsub_multicast\": [\n");
    for (int i = 0; i < 16; ++i) {
        fprintf(fp, "    {\n");
        fprintf(fp, "      \"mode\": \"%s\",\n", m5[i].mode_name);
        fprintf(fp, "      \"fanout\": %u,\n", m5[i].fanout);
        fprintf(fp, "      \"pub_msgs_sec\": %.2f,\n", m5[i].pub_msgs_sec);
        fprintf(fp, "      \"effective_msgs_sec\": %.2f,\n", m5[i].effective_msgs_sec);
        fprintf(fp, "      \"effective_mb_sec\": %.2f\n", m5[i].effective_mb_sec);
        fprintf(fp, "    }%s\n", (i + 1 < 16) ? "," : "");
    }
    fprintf(fp, "  ],\n");

    /* Module 6 */
    fprintf(fp, "  \"module6_simd_scanning\": {\n");
    fprintf(fp, "    \"standard_64bit\": [\n");
    for (int i = 0; i < 4; ++i) {
        fprintf(fp, "      {\"engine\": \"%s\", \"mslots_sec\": %.2f, \"speedup\": %.2f}%s\n",
                m6[i].engine_name, m6[i].std_mslots_sec, m6[i].std_speedup, (i + 1 < 4) ? "," : "");
    }
    fprintf(fp, "    ],\n");
    fprintf(fp, "    \"tier3_packed\": [\n");
    for (int i = 0; i < 4; ++i) {
        fprintf(fp, "      {\"engine\": \"%s\", \"mslots_sec\": %.2f, \"speedup\": %.2f}%s\n",
                m6[i].engine_name, m6[i].packed_mslots_sec, m6[i].packed_speedup, (i + 1 < 4) ? "," : "");
    }
    fprintf(fp, "    ]\n");
    fprintf(fp, "  },\n");

    /* Module 7 */
    fprintf(fp, "  \"module7_primitives\": {\n");
    fprintf(fp, "    \"multi_fd\": {\n");
    fprintf(fp, "      \"count_1\": {\"ops_sec\": %.2f, \"latency_us\": %.2f},\n",
            m7->mfd_1_ops_sec, m7->mfd_1_latency_us);
    fprintf(fp, "      \"count_2\": {\"ops_sec\": %.2f, \"latency_us\": %.2f},\n",
            m7->mfd_2_ops_sec, m7->mfd_2_latency_us);
    fprintf(fp, "      \"count_4\": {\"ops_sec\": %.2f, \"latency_us\": %.2f},\n",
            m7->mfd_4_ops_sec, m7->mfd_4_latency_us);
    fprintf(fp, "      \"count_4_seq\": {\"ops_sec\": %.2f, \"latency_us\": %.2f},\n",
            m7->mfd_4_seq_ops_sec, m7->mfd_4_seq_latency_us);
    fprintf(fp, "      \"speedup\": %.2f\n", m7->mfd_4_speedup);
    fprintf(fp, "    },\n");
    fprintf(fp, "    \"futex2\": {\n");
    fprintf(fp, "      \"memoized\": {\"ops_sec\": %.2f, \"latency_ns\": %.2f},\n",
            m7->futex_memo_ops_sec, m7->futex_memo_lat_ns);
    fprintf(fp, "      \"raw_syscall\": {\"ops_sec\": %.2f, \"latency_ns\": %.2f},\n",
            m7->futex_syscall_ops_sec, m7->futex_syscall_lat_ns);
    fprintf(fp, "      \"speedup\": %.2f\n", m7->futex_speedup);
    fprintf(fp, "    },\n");
    fprintf(fp, "    \"hugetlb_available\": %s\n", m7->hugetlb_available ? "true" : "false");
    fprintf(fp, "  },\n");

    /* Module 8 */
    fprintf(fp, "  \"module8_dynamic_hugepages\": [\n");
    for (int i = 0; i < 4; ++i) {
        fprintf(fp, "    {\n");
        fprintf(fp, "      \"label\": \"%s\",\n", m8[i].label);
        fprintf(fp, "      \"capacity\": %u,\n", m8[i].capacity);
        fprintf(fp, "      \"slot_size\": %u,\n", m8[i].slot_size);
        fprintf(fp, "      \"buffer_mb\": %.2f,\n", m8[i].buffer_mb);
        fprintf(fp, "      \"thp_msgs_sec\": %.2f,\n", m8[i].thp_msgs_sec);
        fprintf(fp, "      \"thp_mb_sec\": %.2f,\n", m8[i].thp_mb_sec);
        fprintf(fp, "      \"thp_lat_ns\": %.2f,\n", m8[i].thp_lat_ns);
        fprintf(fp, "      \"thp_pmd_mapped_kb\": %lu,\n", m8[i].thp_pmd_mapped_kb);
        fprintf(fp, "      \"std_msgs_sec\": %.2f,\n", m8[i].std_msgs_sec);
        fprintf(fp, "      \"std_mb_sec\": %.2f,\n", m8[i].std_mb_sec);
        fprintf(fp, "      \"std_lat_ns\": %.2f,\n", m8[i].std_lat_ns);
        fprintf(fp, "      \"speedup_factor\": %.2f,\n", m8[i].speedup_factor);
        fprintf(fp, "      \"latency_reduction_pct\": %.2f\n", m8[i].latency_reduction_pct);
        fprintf(fp, "    }%s\n", (i + 1 < 4) ? "," : "");
    }
    fprintf(fp, "  ]\n");
    fprintf(fp, "}\n");

    fclose(fp);
    printf("%s%s[Module 9] JSON telemetry written successfully to:%s %s\n",
           COL_BOLD, COL_GREEN, COL_RESET, json_path);
}
