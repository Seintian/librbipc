/**
 * @file bench_crc32c.c
 * @brief Platform-Agnostic Micro-Benchmark for Hardware vs Table-Based CRC32C.
 *
 * Compares the performance of:
 *  1. Hardware-Accelerated Castagnoli CRC32C (x86 SSE4.2 crc32q/crc32b, ARMv8-A CRC32)
 *  2. Direct x86 SSE4.2 Intrinsics (zero dispatch overhead)
 *  3. Table-Based 256-Entry Fallback (rbipc_crc32c_sw)
 *
 * Evaluates performance across buffer geometries from tiny metadata headers (16B, 60B header)
 * to 1MB bulk transfers, evaluating both 64-byte aligned and unaligned memory offsets.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "rbipc.h"
#include "../src/rbipc_arch.h"
#include "bench_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <time.h>

/* Prevent dead-code elimination / loop hoisting of pure CRC computation */
static inline void bench_do_not_optimize(uint32_t val) {
    __asm__ volatile("" : : "r"(val) : "memory");
}

/* Platform detection helpers */
static inline bool bench_has_hw_crc32c(void) {
#if defined(__x86_64__) || defined(__i386__)
    return __builtin_cpu_supports("sse4.2") != 0;
#elif defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
    return true;
#else
    return false;
#endif
}

static inline const char *bench_hw_engine_name(void) {
#if defined(__x86_64__) || defined(__i386__)
    if (__builtin_cpu_supports("sse4.2")) {
        return "x86 SSE4.2 (crc32q / crc32b)";
    }
    return "None (CPU lacks SSE4.2)";
#elif defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
    return "ARMv8-A CRC32 Extension (crc32cd / crc32cb)";
#elif defined(__aarch64__)
    return "None (AArch64 without CRC32 extension)";
#else
    return "None (Unsupported Architecture)";
#endif
}

/* Standard Castagnoli CRC wrappers */
static inline uint32_t bench_crc32c_hw(const void *data, size_t len) {
    return rbipc_crc32c(data, len);
}

#if defined(__x86_64__) || defined(__i386__)
__attribute__((target("sse4.2")))
static inline uint32_t bench_crc32c_direct_sse42(const void *data, size_t len) {
    return ~rbipc_crc32c_sse42(0xFFFFFFFFU, data, len);
}
#endif

static inline uint32_t bench_crc32c_table(const void *data, size_t len) {
    return ~rbipc_crc32c_sw(0xFFFFFFFFU, data, len);
}

typedef struct {
    size_t size;
    const char *label;
    bool is_unaligned;
    uint64_t iterations;

    double hw_mb_s;
    double hw_ns_per_op;

    double direct_hw_mb_s;
    double direct_hw_ns_per_op;

    double table_mb_s;
    double table_ns_per_op;

    double speedup_ratio;
} crc_bench_result_t;

/* Benchmarking execution for a specific implementation */
typedef uint32_t (*crc_fn_t)(const void *data, size_t len);

static void run_single_benchmark(crc_fn_t fn, const void *data, size_t len, uint64_t iters,
                                 double *out_mb_s, double *out_ns_per_op) {
    /* Warm up cache and CPU frequency scaling */
    for (uint64_t i = 0; i < (iters > 1000 ? 500 : iters / 2); i++) {
        uint32_t c = fn(data, len);
        bench_do_not_optimize(c);
    }

    uint64_t start_ns = rbipc_clock_monotonic_ns();
    for (uint64_t i = 0; i < iters; i++) {
        uint32_t c = fn(data, len);
        bench_do_not_optimize(c);
    }
    uint64_t elapsed_ns = rbipc_clock_monotonic_ns() - start_ns;
    if (elapsed_ns == 0) elapsed_ns = 1;

    double total_bytes = (double)len * (double)iters;
    double elapsed_sec = (double)elapsed_ns / 1e9;
    *out_mb_s = (total_bytes / (1024.0 * 1024.0)) / elapsed_sec;
    *out_ns_per_op = (double)elapsed_ns / (double)iters;
}

int main(void) {
    printf("========================================================================================\n");
    printf("            librbipc Castagnoli CRC32C Micro-Benchmark (HW vs. Table)                  \n");
    printf("========================================================================================\n");

    bench_hardware_info_t hw_info;
    bench_get_hardware_info(&hw_info);

    bool hw_supported = bench_has_hw_crc32c();
    const char *engine = bench_hw_engine_name();

    printf("Hardware Context:\n");
    printf("  CPU Model        : %s\n", hw_info.cpu_model);
    printf("  Logical Cores    : %d threads (%d physical)\n", hw_info.logical_threads, hw_info.physical_cores);
    printf("  Kernel           : %s\n", hw_info.kernel);
    printf("  Profile          : %s\n", hw_info.profile_name);
    printf("  HW CRC32 Engine  : %s\n", engine);
    printf("  HW Acceleration  : %s\n", hw_supported ? "SUPPORTED (Active)" : "UNSUPPORTED (Fallback only)");
    printf("----------------------------------------------------------------------------------------\n\n");

    /* Allocate 2MB 64-byte aligned memory buffer for testing */
    size_t max_buf_size = 1024 * 1024 + 64;
    uint8_t *raw_buf = NULL;
    int ret = posix_memalign((void **)&raw_buf, 64, max_buf_size);
    if (ret != 0 || !raw_buf) {
        fprintf(stderr, "Failed to allocate aligned memory buffer\n");
        return 1;
    }

    /* Fill buffer with deterministic pseudo-random sequence */
    for (size_t i = 0; i < max_buf_size; i++) {
        raw_buf[i] = (uint8_t)((i * 137U + 29U) & 0xFFU);
    }

    /* Test configurations */
    struct {
        size_t size;
        const char *label;
        bool unaligned;
        uint64_t default_iters;
        uint64_t laptop_iters;
    } configs[] = {
        { 16,        "16 B (Ticket/Slot Mini)",     false, 20000000, 5000000 },
        { 16,        "16 B [Unaligned +1]",         true,  20000000, 5000000 },
        { 60,        "60 B (Control Header Zone)",  false, 15000000, 4000000 },
        { 60,        "60 B [Unaligned +1]",         true,  15000000, 4000000 },
        { 64,        "64 B (1 Cache Line)",         false, 15000000, 4000000 },
        { 256,       "256 B (Ring Slot Payload)",   false, 10000000, 2000000 },
        { 256,       "256 B [Unaligned +3]",        true,  10000000, 2000000 },
        { 1024,      "1 KiB Buffer",                false,  3000000,  800000 },
        { 4096,      "4 KiB (OS Memory Page)",      false,  1000000,  300000 },
        { 4096,      "4 KiB [Unaligned +1]",        true,   1000000,  300000 },
        { 16384,     "16 KiB Buffer",               false,   300000,   80000 },
        { 65536,     "64 KiB Buffer",               false,    80000,   20000 },
        { 1048576,   "1 MiB Bulk Segment",          false,     5000,    1200 },
    };

    size_t num_configs = sizeof(configs) / sizeof(configs[0]);
    crc_bench_result_t *results = calloc(num_configs, sizeof(crc_bench_result_t));
    if (!results) {
        free(raw_buf);
        return 1;
    }

    printf("Executing benchmark runs across %zu buffer geometries...\n\n", num_configs);

    for (size_t i = 0; i < num_configs; i++) {
        size_t size = configs[i].size;
        bool unaligned = configs[i].unaligned;
        const uint8_t *ptr = unaligned ? (raw_buf + (size == 256 ? 3 : 1)) : raw_buf;
        uint64_t iters = bench_scale_iterations(configs[i].default_iters, configs[i].laptop_iters);

        results[i].size = size;
        results[i].label = configs[i].label;
        results[i].is_unaligned = unaligned;
        results[i].iterations = iters;

        /* Verify numerical correctness between HW and Table implementations */
        uint32_t val_table = bench_crc32c_table(ptr, size);
        uint32_t val_hw = bench_crc32c_hw(ptr, size);
        if (val_table != val_hw) {
            fprintf(stderr, "CORRUPTION ERROR for size %zu: Table=0x%08X vs HW=0x%08X\n", size, val_table, val_hw);
            assert(val_table == val_hw);
        }

#if defined(__x86_64__) || defined(__i386__)
        if (hw_supported) {
            uint32_t val_direct = bench_crc32c_direct_sse42(ptr, size);
            assert(val_direct == val_hw);
            run_single_benchmark(bench_crc32c_direct_sse42, ptr, size, iters,
                                 &results[i].direct_hw_mb_s, &results[i].direct_hw_ns_per_op);
        }
#endif

        if (hw_supported) {
            run_single_benchmark(bench_crc32c_hw, ptr, size, iters,
                                 &results[i].hw_mb_s, &results[i].hw_ns_per_op);
        } else {
            results[i].hw_mb_s = 0.0;
            results[i].hw_ns_per_op = 0.0;
        }

        run_single_benchmark(bench_crc32c_table, ptr, size, iters,
                             &results[i].table_mb_s, &results[i].table_ns_per_op);

        if (hw_supported && results[i].table_mb_s > 0.0) {
            results[i].speedup_ratio = results[i].hw_mb_s / results[i].table_mb_s;
        } else {
            results[i].speedup_ratio = 1.0;
        }
    }

    /* Print Comparative Table */
    printf("+------------------------------+---------------+-------------------+-------------------+----------+\n");
    printf("| Buffer Configuration         | Size (Bytes)  | HW SSE4.2 (MB/s)  | Table SW (MB/s)   | Speedup  |\n");
    printf("+------------------------------+---------------+-------------------+-------------------+----------+\n");
    for (size_t i = 0; i < num_configs; i++) {
        printf("| %-28s | %13zu | %14.2f MB/s | %14.2f MB/s | %7.2fx |\n",
               results[i].label,
               results[i].size,
               results[i].hw_mb_s,
               results[i].table_mb_s,
               results[i].speedup_ratio);
    }
    printf("+------------------------------+---------------+-------------------+-------------------+----------+\n\n");

    /* Print Latency Breakdown Table */
    printf("+------------------------------+---------------+-------------------+-------------------+\n");
    printf("| Buffer Configuration         | Size (Bytes)  | HW Latency (ns)   | SW Latency (ns)   |\n");
    printf("+------------------------------+---------------+-------------------+-------------------+\n");
    for (size_t i = 0; i < num_configs; i++) {
        printf("| %-28s | %13zu | %14.2f ns   | %14.2f ns   |\n",
               results[i].label,
               results[i].size,
               results[i].hw_ns_per_op,
               results[i].table_ns_per_op);
    }
    printf("+------------------------------+---------------+-------------------+-------------------+\n\n");

    /* Write JSON Results */
    char json_path[256];
    bench_get_result_path(json_path, sizeof(json_path), "bench_crc32c.json");
    FILE *fp = fopen(json_path, "w");
    if (fp) {
        fprintf(fp, "{\n");
        fprintf(fp, "  \"metadata\": {\n");
        fprintf(fp, "    \"cpu_model\": \"%s\",\n", hw_info.cpu_model);
        fprintf(fp, "    \"kernel\": \"%s\",\n", hw_info.kernel);
        fprintf(fp, "    \"profile\": \"%s\",\n", hw_info.profile_name);
        fprintf(fp, "    \"hw_engine\": \"%s\",\n", engine);
        fprintf(fp, "    \"hw_supported\": %s\n", hw_supported ? "true" : "false");
        fprintf(fp, "  },\n");
        fprintf(fp, "  \"benchmarks\": [\n");
        for (size_t i = 0; i < num_configs; i++) {
            fprintf(fp, "    {\n");
            fprintf(fp, "      \"label\": \"%s\",\n", results[i].label);
            fprintf(fp, "      \"size_bytes\": %zu,\n", results[i].size);
            fprintf(fp, "      \"unaligned\": %s,\n", results[i].is_unaligned ? "true" : "false");
            fprintf(fp, "      \"iterations\": %lu,\n", (unsigned long)results[i].iterations);
            fprintf(fp, "      \"hw_throughput_mb_s\": %.2f,\n", results[i].hw_mb_s);
            fprintf(fp, "      \"hw_latency_ns\": %.2f,\n", results[i].hw_ns_per_op);
            fprintf(fp, "      \"direct_hw_throughput_mb_s\": %.2f,\n", results[i].direct_hw_mb_s);
            fprintf(fp, "      \"direct_hw_latency_ns\": %.2f,\n", results[i].direct_hw_ns_per_op);
            fprintf(fp, "      \"table_throughput_mb_s\": %.2f,\n", results[i].table_mb_s);
            fprintf(fp, "      \"table_latency_ns\": %.2f,\n", results[i].table_ns_per_op);
            fprintf(fp, "      \"speedup\": %.2f\n", results[i].speedup_ratio);
            fprintf(fp, "    }%s\n", (i + 1 < num_configs) ? "," : "");
        }
        fprintf(fp, "  ]\n");
        fprintf(fp, "}\n");
        fclose(fp);
        printf("Telemetry exported to %s\n", json_path);
    } else {
        fprintf(stderr, "Warning: Could not open %s for writing\n", json_path);
    }

    free(results);
    free(raw_buf);
    return 0;
}
