/**
 * @file test_unit_latency.c
 * @brief Unit tests for latency distribution percentile calculation and regression testing
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "rbipc.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <math.h>

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
} test_latency_dist_t;

static int compare_uint32_test(const void *a, const void *b) {
    uint32_t val_a = *(const uint32_t *)a;
    uint32_t val_b = *(const uint32_t *)b;
    if (val_a < val_b) return -1;
    if (val_a > val_b) return 1;
    return 0;
}

static void compute_latency_distribution(uint32_t *samples, size_t n, test_latency_dist_t *out) {
    assert(samples != NULL);
    assert(n > 0);

    qsort(samples, n, sizeof(uint32_t), compare_uint32_test);

    double sum = 0.0;
    for (size_t i = 0; i < n; ++i) {
        sum += (double)samples[i];
    }
    double mean = sum / (double)n;

    double var_sum = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double d = (double)samples[i] - mean;
        var_sum += d * d;
    }

    out->min_ns    = samples[0];
    out->mean_ns   = mean;
    out->p50_ns    = samples[(n * 50) / 100];
    out->p90_ns    = samples[(n * 90) / 100];
    out->p99_ns    = samples[(n * 99) / 100];
    out->p99_9_ns  = samples[(n * 999) / 1000];
    out->p99_99_ns = samples[(n * 9999) / 10000];
    out->max_ns    = samples[n - 1];
    out->stddev_ns = sqrt(var_sum / (double)n);
}

static bool check_regression_sla(const test_latency_dist_t *dist, uint32_t max_p99_9_us, uint32_t max_lat_us) {
    uint32_t p99_9_us = (dist->p99_9_ns + 999U) / 1000U;
    uint32_t max_us   = (dist->max_ns + 999U) / 1000U;
    return (p99_9_us <= max_p99_9_us) && (max_us <= max_lat_us);
}

static void test_synthetic_percentiles(void) {
    const size_t N = 100000;
    uint32_t *samples = malloc(sizeof(uint32_t) * N);
    assert(samples != NULL);

    /* Construct controlled distribution:
     * 0 .. 49999 (50%)     : 300 ns
     * 50000 .. 89999 (40%) : 600 ns
     * 90000 .. 98999 (9%)  : 1500 ns
     * 99000 .. 99899 (0.9%): 4000 ns
     * 99900 .. 99989 (0.09%): 15000 ns
     * 99990 .. 99999 (0.01%): 42000 ns
     */
    for (size_t i = 0; i < 50000; ++i) samples[i] = 300;
    for (size_t i = 50000; i < 90000; ++i) samples[i] = 600;
    for (size_t i = 90000; i < 99000; ++i) samples[i] = 1500;
    for (size_t i = 99000; i < 99900; ++i) samples[i] = 4000;
    for (size_t i = 99900; i < 99990; ++i) samples[i] = 15000;
    for (size_t i = 99990; i < 100000; ++i) samples[i] = 42000;

    test_latency_dist_t dist;
    compute_latency_distribution(samples, N, &dist);

    assert(dist.min_ns == 300);
    assert(dist.p50_ns == 600 || dist.p50_ns == 300);
    assert(dist.p90_ns == 1500);
    assert(dist.p99_ns == 4000);
    assert(dist.p99_9_ns == 15000); /* 15 us */
    assert(dist.max_ns == 42000);   /* 42 us */

    /* Verify SLA check passes against reference thresholds (p99.9 <= 50 us, max <= 100 us) */
    assert(check_regression_sla(&dist, 50, 100) == true);

    free(samples);
}

static void test_regression_violation_detection(void) {
    const size_t N = 10000;
    uint32_t *samples = malloc(sizeof(uint32_t) * N);
    assert(samples != NULL);

    /* 9,990 samples at 1 us (99.9%) */
    for (size_t i = 0; i < 9990; ++i) {
        samples[i] = 1000; /* 1 us */
    }

    /* 9 samples at 75 us (p99.9 tail) */
    for (size_t i = 9990; i < N - 1; ++i) {
        samples[i] = 75000; /* 75 us */
    }
    /* 1 sample at 120 us (max tail) */
    samples[N - 1] = 120000; /* 120 us */

    test_latency_dist_t dist;
    compute_latency_distribution(samples, N, &dist);

    assert(dist.p99_9_ns == 75000);
    assert(dist.max_ns == 120000);

    /* Should fail default SLA (50us / 100us) */
    assert(check_regression_sla(&dist, 50, 100) == false);

    /* Should pass with relaxed thresholds */
    assert(check_regression_sla(&dist, 80, 150) == true);

    free(samples);
}

int main(void) {
    printf("[test_unit_latency] Running latency percentile and regression tests...\n");
    test_synthetic_percentiles();
    test_regression_violation_detection();
    printf("[test_unit_latency] All tests passed successfully.\n");
    return 0;
}
