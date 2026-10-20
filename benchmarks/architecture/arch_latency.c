/**
 * @file opt_latency.c
 * @brief Module 1: 3-Tier Latency Hierarchy (Half-RTT Ping-Pong distribution).
 */

#include "arch_common.h"

void run_module1_ping_pong(uint64_t iterations, uint64_t warmup,
                          m1_latency_result_t results[4]) {
    arch_print_subhdr("Module 1: 3-Tier Latency Hierarchy (Half-RTT Ping-Pong)");
    printf("Iterations: %lu (Warmup: %lu) | Pinned: Producer (CPU 2) <-> Consumer (CPU 3)\n\n",
           (unsigned long)iterations, (unsigned long)warmup);

    const struct {
        const char *name;
        uint32_t flags;
    } modes[4] = {
        { "Tier 1 Zero-Trust (Default / Robust)", 0 },
        { "Tier 2 Trusted Mode (Pure Vyukov)",   RBIPC_FLAG_TRUSTED },
        { "Tier 3 Packed Descriptor Mode",      RBIPC_FLAG_PACKED },
        { "Combined Tier 2+3 (Trusted & Packed)", RBIPC_FLAG_TRUSTED | RBIPC_FLAG_PACKED }
    };

    uint64_t total_rounds = warmup + iterations;

    for (int m = 0; m < 4; ++m) {
        results[m].mode_name = modes[m].name;
        results[m].flags = modes[m].flags;

        rbipc_destroy(ARCH_SHM_LAT_A);
        rbipc_destroy(ARCH_SHM_LAT_B);

        rbipc_ring_t *ring_a = NULL;
        rbipc_ring_t *ring_b = NULL;
        assert(rbipc_create_ex(ARCH_SHM_LAT_A, 1024, 64, RBIPC_SHM_NAMED | modes[m].flags, 0660, &ring_a) == RBIPC_OK);
        assert(rbipc_create_ex(ARCH_SHM_LAT_B, 1024, 64, RBIPC_SHM_NAMED | modes[m].flags, 0660, &ring_b) == RBIPC_OK);

        fflush(stdout);
        pid_t pid = fork();
        assert(pid >= 0);

        if (pid == 0) {
            arch_pin_to_core(3);
            rbipc_ring_t *c_a = NULL;
            rbipc_ring_t *c_b = NULL;
            assert(rbipc_attach(ARCH_SHM_LAT_A, &c_a) == RBIPC_OK);
            assert(rbipc_attach(ARCH_SHM_LAT_B, &c_b) == RBIPC_OK);

            for (uint64_t i = 0; i < total_rounds; ++i) {
                const void *rbuf = NULL;
                uint32_t rlen = 0;
                uint64_t rticket = 0;
                int rc = rbipc_read_acquire(c_a, &rbuf, &rlen, &rticket);
                if (rc != RBIPC_OK) break;
                uint64_t ts = *(const uint64_t *)rbuf;
                rbipc_read_release(c_a, rticket);

                void *wbuf = NULL;
                uint64_t wticket = 0;
                rc = rbipc_reserve_write(c_b, sizeof(uint64_t), &wbuf, &wticket);
                if (rc != RBIPC_OK) break;
                *(uint64_t *)wbuf = ts;
                rc = rbipc_commit_write(c_b, wticket, sizeof(uint64_t));
                if (rc != RBIPC_OK) break;
            }

            rbipc_detach(c_a);
            rbipc_detach(c_b);
            _exit(0);
        }

        arch_pin_to_core(2);
        uint32_t *samples = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)iterations);
        assert(samples != NULL);

        double sum = 0.0;
        for (uint64_t i = 0; i < total_rounds; ++i) {
            uint64_t t0 = arch_get_time_ns();
            void *wbuf = NULL;
            uint64_t wticket = 0;
            assert(rbipc_reserve_write(ring_a, sizeof(uint64_t), &wbuf, &wticket) == RBIPC_OK);
            *(uint64_t *)wbuf = t0;
            assert(rbipc_commit_write(ring_a, wticket, sizeof(uint64_t)) == RBIPC_OK);

            const void *rbuf = NULL;
            uint32_t rlen = 0;
            uint64_t rticket = 0;
            assert(rbipc_read_acquire(ring_b, &rbuf, &rlen, &rticket) == RBIPC_OK);
            uint64_t t1 = arch_get_time_ns();
            rbipc_read_release(ring_b, rticket);

            if (i >= warmup) {
                uint64_t rtt = (t1 > t0) ? (t1 - t0) : 1;
                uint32_t half_rtt = (uint32_t)(rtt / 2);
                samples[i - warmup] = half_rtt;
                sum += (double)half_rtt;
            }
        }

        waitpid(pid, NULL, 0);
        arch_unpin_core();

        rbipc_detach(ring_a);
        rbipc_detach(ring_b);
        rbipc_destroy(ARCH_SHM_LAT_A);
        rbipc_destroy(ARCH_SHM_LAT_B);

        qsort(samples, (size_t)iterations, sizeof(uint32_t), arch_compare_uint32);

        results[m].min_ns    = samples[0];
        results[m].p25_ns   = samples[(size_t)(iterations * 0.25)];
        results[m].p50_ns   = samples[(size_t)(iterations * 0.50)];
        results[m].p75_ns   = samples[(size_t)(iterations * 0.75)];
        results[m].p90_ns   = samples[(size_t)(iterations * 0.90)];
        results[m].p99_ns   = samples[(size_t)(iterations * 0.99)];
        results[m].p99_9_ns = samples[(size_t)(iterations * 0.999)];
        results[m].max_ns    = samples[iterations - 1];
        results[m].mean_ns   = sum / (double)iterations;

        if (m == 0) {
            results[m].reduction_pct = 0.0;
        } else {
            results[m].reduction_pct = ((double)results[0].p50_ns - (double)results[m].p50_ns) / (double)results[0].p50_ns * 100.0;
        }

        free(samples);

        printf("  %-40s : Min: %3u ns | P50: %3u ns | P99: %4u ns | Mean: %6.1f ns | P50 Gain: %+.1f%%\n",
               results[m].mode_name, results[m].min_ns, results[m].p50_ns,
               results[m].p99_ns, results[m].mean_ns, results[m].reduction_pct);
    }
    printf("\n");
}
