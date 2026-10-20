/**
 * @file suite_latency.c
 * @brief Ping-pong round-trip latency distribution and SLA audit
 */

#include "suite_common.h"

void run_bench_ping_pong(int iterations, latency_dist_t *out_dist) {
    printf("\n[2/6] Running Ping-Pong RTT Latency Distribution (%d rounds)...\n", iterations);
    rbipc_destroy(BENCH_SHM_A);
    rbipc_destroy(BENCH_SHM_B);

    rbipc_ring_t *ring_a = NULL;
    rbipc_ring_t *ring_b = NULL;
    int rc = rbipc_create(BENCH_SHM_A, 256, 64, &ring_a);
    assert(rc == RBIPC_OK);
    rc = rbipc_create(BENCH_SHM_B, 256, 64, &ring_b);
    assert(rc == RBIPC_OK);

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(1);
    }

    if (pid == 0) {
        /* Echo Server Process: Read from A, write back to B */
        rbipc_ring_t *c_a = NULL, *c_b = NULL;
        assert(rbipc_attach(BENCH_SHM_A, &c_a) == RBIPC_OK);
        assert(rbipc_attach(BENCH_SHM_B, &c_b) == RBIPC_OK);

        for (int i = 0; i < iterations; ++i) {
            const void *rbuf = NULL;
            uint32_t rlen = 0;
            uint64_t rticket = 0;
            rc = rbipc_read_acquire(c_a, &rbuf, &rlen, &rticket);
            if (rc != RBIPC_OK) break;

            uint64_t ts = *(const uint64_t *)rbuf;
            rbipc_read_release(c_a, rticket);

            void *wbuf = NULL;
            uint64_t wticket = 0;
            assert(rbipc_reserve_write(c_b, sizeof(uint64_t), &wbuf, &wticket) == RBIPC_OK);
            *(uint64_t *)wbuf = ts;
            assert(rbipc_commit_write(c_b, wticket, sizeof(uint64_t)) == RBIPC_OK);
        }

        rbipc_detach(c_a);
        rbipc_detach(c_b);
        _exit(0);
    }

    /* Client Process: Send to A, receive from B, record RTT / 2 */
    uint32_t *samples = malloc(sizeof(uint32_t) * (size_t)iterations);
    assert(samples != NULL);

    double sum = 0;
    for (int i = 0; i < iterations; ++i) {
        uint64_t t0 = get_time_ns();
        void *wbuf = NULL;
        uint64_t wticket = 0;

        assert(rbipc_reserve_write(ring_a, sizeof(uint64_t), &wbuf, &wticket) == RBIPC_OK);
        *(uint64_t *)wbuf = t0;
        assert(rbipc_commit_write(ring_a, wticket, sizeof(uint64_t)) == RBIPC_OK);

        const void *rbuf = NULL;
        uint32_t rlen = 0;
        uint64_t rticket = 0;
        assert(rbipc_read_acquire(ring_b, &rbuf, &rlen, &rticket) == RBIPC_OK);
        uint64_t t1 = get_time_ns();
        rbipc_read_release(ring_b, rticket);

        uint64_t rtt = (t1 > t0) ? (t1 - t0) : 1;
        uint32_t one_way = (uint32_t)(rtt / 2);
        samples[i] = one_way;
        sum += (double)one_way;
    }

    waitpid(pid, NULL, 0);
    rbipc_detach(ring_a);
    rbipc_detach(ring_b);
    rbipc_destroy(BENCH_SHM_A);
    rbipc_destroy(BENCH_SHM_B);

    qsort(samples, (size_t)iterations, sizeof(uint32_t), compare_uint32);

    double mean = sum / (double)iterations;
    double var_sum = 0;
    for (int i = 0; i < iterations; ++i) {
        double diff = (double)samples[i] - mean;
        var_sum += diff * diff;
    }
    double stddev = sqrt(var_sum / (double)iterations);

    out_dist->min_ns     = samples[0];
    out_dist->mean_ns    = mean;
    out_dist->p50_ns     = samples[(size_t)(iterations * 0.50)];
    out_dist->p90_ns     = samples[(size_t)(iterations * 0.90)];
    out_dist->p99_ns     = samples[(size_t)(iterations * 0.99)];
    out_dist->p99_9_ns   = samples[(size_t)(iterations * 0.999)];
    out_dist->p99_99_ns  = samples[(size_t)(iterations * 0.9999)];
    out_dist->max_ns     = samples[iterations - 1];
    out_dist->stddev_ns  = stddev;

    free(samples);

    printf("  Min Latency     : %u ns\n", out_dist->min_ns);
    printf("  Mean Latency    : %.2f ns (stddev: %.2f ns)\n", out_dist->mean_ns, out_dist->stddev_ns);
    printf("  Median (p50)    : %u ns\n", out_dist->p50_ns);
    printf("  p90 Latency     : %u ns\n", out_dist->p90_ns);
    printf("  p99 Latency     : %u ns\n", out_dist->p99_ns);
    printf("  p99.9 Latency   : %u ns\n", out_dist->p99_9_ns);
    printf("  p99.99 Latency  : %u ns\n", out_dist->p99_99_ns);
    printf("  Max Latency     : %u ns\n", out_dist->max_ns);
}

bool verify_latency_regression(const latency_dist_t *dist, bool strict,
                              uint32_t override_p99_9_us, uint32_t override_max_us,
                              latency_sla_result_t *out_sla) {
    const char *p99_env = getenv("RBIPC_BENCH_MAX_P99_9_US");
    const char *max_env = getenv("RBIPC_BENCH_MAX_US");

    uint32_t max_p99_9_us = override_p99_9_us ? override_p99_9_us :
                            (p99_env ? (uint32_t)strtoul(p99_env, NULL, 10) : RBIPC_DEFAULT_MAX_P99_9_US);
    uint32_t max_lat_us   = override_max_us ? override_max_us :
                            (max_env ? (uint32_t)strtoul(max_env, NULL, 10) : RBIPC_DEFAULT_MAX_LAT_US);

    uint32_t p99_9_us = (dist->p99_9_ns + 999U) / 1000U;
    uint32_t max_us   = (dist->max_ns + 999U) / 1000U;

    bool p99_pass = (p99_9_us <= max_p99_9_us);
    bool max_pass = (max_us <= max_lat_us);
    bool overall_pass = p99_pass && max_pass;

    if (out_sla) {
        out_sla->threshold_p99_9_us = max_p99_9_us;
        out_sla->threshold_max_us   = max_lat_us;
        out_sla->observed_p99_9_us  = p99_9_us;
        out_sla->observed_max_us    = max_us;
        out_sla->passed             = overall_pass;
    }

    printf("\n--> [5.3 Latency Percentile Regression SLA Audit]\n");
    printf("    Target p99.9 Ceiling: <= %u us | Observed: %u us (%u ns)\n",
           max_p99_9_us, p99_9_us, dist->p99_9_ns);
    printf("    Target Max Ceiling  : <= %u us | Observed: %u us (%u ns)\n",
           max_lat_us, max_us, dist->max_ns);

    if (overall_pass) {
        printf("    [PASS] Latency distribution satisfies SLA performance ceilings.\n");
        return true;
    }

    if (strict) {
        fprintf(stderr, "\n    [FAIL] Latency percentile regression threshold exceeded.\n");
        if (!p99_pass) {
            fprintf(stderr, "           Observed p99.9 (%u us) > threshold (%u us)\n", p99_9_us, max_p99_9_us);
        }
        if (!max_pass) {
            fprintf(stderr, "           Observed max (%u us) > threshold (%u us)\n", max_us, max_lat_us);
        }
        return false;
    } else {
        printf("    [NOTE] Latency tail exceeded reference SLA ceilings.\n");
        printf("           (Expected under virtualization/WSL2; enforce via --strict or RBIPC_BENCH_STRICT_REGRESSION=1 in CI)\n");
        return true;
    }
}
