/**
 * @file deep_common.h
 * @brief Common definitions, data structures, and declarations for the deep dive micro-benchmarks
 */

#ifndef DEEP_COMMON_H
#define DEEP_COMMON_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "rbipc.h"
#include "rbipc_internal.h"
#include "rbipc_simd.h"
#include "../bench_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <sys/wait.h>
#include <sys/epoll.h>
#include <assert.h>
#include <math.h>

#define DEEP_SHM_A "/rbipc_deep_shm_a"
#define DEEP_SHM_B "/rbipc_deep_shm_b"
#define DEEP_PUBSUB "/rbipc_deep_pubsub"

static inline uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * RBIPC_NSEC_PER_SEC) + (uint64_t)ts.tv_nsec;
}

static inline int compare_u32(const void *a, const void *b) {
    uint32_t val_a = *(const uint32_t *)a;
    uint32_t val_b = *(const uint32_t *)b;
    if (val_a < val_b) return -1;
    if (val_a > val_b) return 1;
    return 0;
}

/* Category 1: Adaptive Backoff metrics */
typedef struct {
    double msgs_per_sec;
    uint32_t p50_ns;
    uint32_t p90_ns;
    uint32_t p99_ns;
    uint32_t max_ns;
} backoff_bench_result_t;

/* Category 2: Non-Temporal Streaming Store metrics */
typedef struct {
    uint32_t payload_size;
    double std_mb_per_sec;
    double stream_mb_per_sec;
    double speedup_pct;
} stream_store_bench_t;

/* Category 3: PubSub Multicast Fanout metrics */
typedef struct {
    uint32_t num_subscribers;
    double msgs_per_sec;
    double mb_per_sec;
    double effective_delivery_rate;
} pubsub_fanout_bench_t;

/* Category 4: Reactor / Eventfd Latency metrics */
typedef struct {
    double events_per_sec;
    double avg_latency_ns;
} reactor_bench_result_t;

/* Category 5: SIMD Vectorized Scanning metrics */
typedef struct {
    double scalar_ns_per_batch;
    double simd_ns_per_batch;
    double speedup_pct;
} simd_scan_bench_t;

/* Category 6: Consumer Lease Overhead metrics */
typedef struct {
    double no_lease_msgs_per_sec;
    double with_lease_msgs_per_sec;
    double overhead_pct;
} lease_bench_result_t;

/* Function prototypes */
void run_backoff_comparison(uint32_t policy, backoff_bench_result_t *out_res);
void run_stream_store_bench(uint32_t payload_size, int iterations, stream_store_bench_t *out);
void run_pubsub_fanout_bench(uint32_t num_subs, int iterations, pubsub_fanout_bench_t *out);
void run_reactor_bench(int iterations, reactor_bench_result_t *out);
void run_simd_scan_bench(int iterations, simd_scan_bench_t *out);
void run_lease_overhead_bench(int iterations, lease_bench_result_t *out);

void export_deep_dive_json(const backoff_bench_result_t *zero_spin,
                           const backoff_bench_result_t *adaptive,
                           const stream_store_bench_t *stream_res, size_t num_sp,
                           const pubsub_fanout_bench_t *pubsub_res, size_t num_subs,
                           const reactor_bench_result_t *reactor_res,
                           const simd_scan_bench_t *simd_res,
                           const lease_bench_result_t *lease_res);

#endif /* DEEP_COMMON_H */
