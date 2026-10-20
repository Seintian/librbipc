/**
 * @file suite_common.h
 * @brief Common definitions, data structures, and declarations for the main benchmark suite
 */

#ifndef SUITE_COMMON_H
#define SUITE_COMMON_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "rbipc.h"
#include "../bench_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <assert.h>
#include <math.h>

#define BENCH_SHM_A "/rbipc_bench_shm_a"
#define BENCH_SHM_B "/rbipc_bench_shm_b"

#define RBIPC_DEFAULT_MAX_P99_9_US  50U
#define RBIPC_DEFAULT_MAX_LAT_US    100U

static inline uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * RBIPC_NSEC_PER_SEC) + (uint64_t)ts.tv_nsec;
}

static inline int compare_uint32(const void *a, const void *b) {
    uint32_t val_a = *(const uint32_t *)a;
    uint32_t val_b = *(const uint32_t *)b;
    if (val_a < val_b) return -1;
    if (val_a > val_b) return 1;
    return 0;
}

/* Category 1: Idle waiting metrics */
typedef struct {
    double user_sec;
    double sys_sec;
    double total_sec;
    double cpu_pct;
    long voluntary_ctxt_switches;
    long involuntary_ctxt_switches;
} idle_metrics_t;

/* Category 2: Latency distribution metrics */
typedef struct {
    uint32_t min_ns;
    double   mean_ns;
    uint32_t p50_ns;
    uint32_t p90_ns;
    uint32_t p99_ns;
    uint32_t p99_9_ns;
    uint32_t p99_99_ns;
    uint32_t max_ns;
    double   stddev_ns;
} latency_dist_t;

typedef struct {
    uint32_t threshold_p99_9_us;
    uint32_t threshold_max_us;
    uint32_t observed_p99_9_us;
    uint32_t observed_max_us;
    bool     passed;
} latency_sla_result_t;

/* Category 3: Streaming IPC throughput metrics */
typedef struct {
    int iterations;
    double elapsed_sec;
    double msgs_per_sec;
    double mb_per_sec;
    double avg_latency_ns;
    long producer_nvcsw;
    long producer_nivcsw;
    long consumer_nvcsw;
    long consumer_nivcsw;
} stream_metrics_t;

/* Category 4: Batch scaling metrics */
typedef struct {
    uint32_t batch_size;
    double   msgs_per_sec;
    double   mb_per_sec;
    double   avg_latency_ns;
} batch_result_t;

/* Category 5: Payload size scaling metrics */
typedef struct {
    uint32_t payload_size;
    double   msgs_per_sec;
    double   mb_per_sec;
} payload_result_t;

/* Category 6: MPMC scalability metrics */
typedef struct {
    uint32_t num_prod;
    uint32_t num_cons;
    double   msgs_per_sec;
    double   avg_latency_ns;
} mpmc_result_t;

/* Function prototypes */
void run_bench_idle(idle_metrics_t *out_metrics);
void run_bench_ping_pong(int iterations, latency_dist_t *out_dist);
bool verify_latency_regression(const latency_dist_t *dist, bool strict,
                              uint32_t override_p99_9_us, uint32_t override_max_us,
                              latency_sla_result_t *out_sla);
void run_bench_streaming(int iterations, stream_metrics_t *out_metrics);
void run_bench_batching(uint32_t batch_size, int total_msgs, batch_result_t *out_res);
void run_bench_payload(uint32_t payload_size, int count, payload_result_t *out_res);
void run_bench_mpmc(uint32_t np, uint32_t nc, uint32_t total, mpmc_result_t *out_res);

void export_suite_json(const char *tag,
                       const idle_metrics_t *idle,
                       const latency_dist_t *lat,
                       const latency_sla_result_t *sla,
                       const stream_metrics_t *stream,
                       const batch_result_t *batch_results, size_t num_batches,
                       const payload_result_t *payload_results, size_t num_payloads,
                       const mpmc_result_t *mpmc_results, size_t num_mpmc);

#endif /* SUITE_COMMON_H */
