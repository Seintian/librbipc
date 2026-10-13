/**
 * @file bench_deep_dive.c
 * @brief Comparative Micro-Benchmark Suite for librbipc
 *
 * Specifically quantifies the performance impact of Phase 1-5 architectural changes:
 *  1. Adaptive Backoff (RBIPC_WAIT_ADAPTIVE) vs Pure Passive (RBIPC_WAIT_ZERO_SPIN)
 *  2. Non-Temporal Streaming Stores (rbipc_write_stream) vs Standard Cached Stores
 *  3. 1-to-N Multicast PubSub Engine Fanout (1P -> 1C, 2C, 4C)
 *  4. Asynchronous Eventfd Reactor Notification Latency
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "rbipc.h"
#include "../src/rbipc_internal.h"
#include "bench_common.h"
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

static int compare_u32(const void *a, const void *b) {
    uint32_t val_a = *(const uint32_t *)a;
    uint32_t val_b = *(const uint32_t *)b;
    if (val_a < val_b) return -1;
    if (val_a > val_b) return 1;
    return 0;
}

/* ============================================================================
 * 1. Adaptive Backoff vs Zero-Spin Passive Sleep
 * ============================================================================ */
typedef struct {
    double msgs_per_sec;
    uint32_t p50_ns;
    uint32_t p90_ns;
    uint32_t p99_ns;
    uint32_t max_ns;
} backoff_bench_result_t;

static void run_backoff_comparison(uint32_t policy, backoff_bench_result_t *out_res) {
    rbipc_destroy(DEEP_SHM_A);
    rbipc_destroy(DEEP_SHM_B);

    rbipc_ring_t *ring_a = NULL;
    rbipc_ring_t *ring_b = NULL;
    assert(rbipc_create(DEEP_SHM_A, 256, 64, &ring_a) == RBIPC_OK);
    assert(rbipc_create(DEEP_SHM_B, 256, 64, &ring_b) == RBIPC_OK);

    rbipc_set_wait_policy(ring_a, policy);
    rbipc_set_wait_policy(ring_b, policy);

    int iterations = 50000;

    pid_t pid = fork();
    if (pid == 0) {
        rbipc_ring_t *c_a = NULL, *c_b = NULL;
        assert(rbipc_attach(DEEP_SHM_A, &c_a) == RBIPC_OK);
        assert(rbipc_attach(DEEP_SHM_B, &c_b) == RBIPC_OK);
        rbipc_set_wait_policy(c_a, policy);
        rbipc_set_wait_policy(c_b, policy);

        for (int i = 0; i < iterations; ++i) {
            const void *rbuf = NULL;
            uint32_t rlen = 0;
            uint64_t rticket = 0;
            if (rbipc_read_acquire(c_a, &rbuf, &rlen, &rticket) != RBIPC_OK) break;
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

    uint32_t *samples = malloc(sizeof(uint32_t) * (size_t)iterations);
    assert(samples != NULL);

    uint64_t t_start = get_time_ns();
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
        samples[i] = (uint32_t)(rtt / 2);
    }
    uint64_t t_end = get_time_ns();

    waitpid(pid, NULL, 0);
    rbipc_detach(ring_a);
    rbipc_detach(ring_b);
    rbipc_destroy(DEEP_SHM_A);
    rbipc_destroy(DEEP_SHM_B);

    qsort(samples, (size_t)iterations, sizeof(uint32_t), compare_u32);

    double elapsed_sec = (double)(t_end - t_start) / (double)RBIPC_NSEC_PER_SEC;
    out_res->msgs_per_sec = (double)(iterations * 2) / elapsed_sec;
    out_res->p50_ns       = samples[(size_t)(iterations * 0.50)];
    out_res->p90_ns       = samples[(size_t)(iterations * 0.90)];
    out_res->p99_ns       = samples[(size_t)(iterations * 0.99)];
    out_res->max_ns       = samples[iterations - 1];

    free(samples);
}

/* ============================================================================
 * 2. Non-Temporal Streaming Stores vs Standard Cached Writes
 * ============================================================================ */
typedef struct {
    uint32_t payload_size;
    double std_mb_per_sec;
    double stream_mb_per_sec;
    double speedup_pct;
} stream_store_bench_t;

static void run_stream_store_bench(uint32_t payload_size, int iterations, stream_store_bench_t *out) {
    rbipc_destroy(DEEP_SHM_A);
    rbipc_ring_t *ring = NULL;
    assert(rbipc_create(DEEP_SHM_A, 64, payload_size, &ring) == RBIPC_OK);

    void *src_buffer = malloc(payload_size);
    memset(src_buffer, 0xAB, payload_size);

    /* Test 1: Standard write (memcpy) */
    uint64_t t0 = get_time_ns();
    for (int i = 0; i < iterations; ++i) {
        void *wbuf = NULL;
        uint64_t wticket = 0;
        assert(rbipc_reserve_write(ring, payload_size, &wbuf, &wticket) == RBIPC_OK);
        memcpy(wbuf, src_buffer, payload_size);
        assert(rbipc_commit_write(ring, wticket, payload_size) == RBIPC_OK);

        const void *rbuf = NULL;
        uint32_t rlen = 0;
        uint64_t rticket = 0;
        assert(rbipc_read_acquire(ring, &rbuf, &rlen, &rticket) == RBIPC_OK);
        rbipc_read_release(ring, rticket);
    }
    uint64_t t1 = get_time_ns();

    double sec_std = (double)(t1 - t0) / (double)RBIPC_NSEC_PER_SEC;
    double mb_std = ((double)iterations * payload_size) / (1024.0 * 1024.0 * sec_std);

    /* Test 2: Non-temporal streaming write */
    uint64_t t2 = get_time_ns();
    for (int i = 0; i < iterations; ++i) {
        uint64_t stream_ticket = 0;
        int rc = rbipc_write_stream(ring, src_buffer, payload_size, &stream_ticket);
        assert(rc == RBIPC_OK);

        const void *rbuf = NULL;
        uint32_t rlen = 0;
        uint64_t rticket = 0;
        assert(rbipc_read_acquire(ring, &rbuf, &rlen, &rticket) == RBIPC_OK);
        rbipc_read_release(ring, rticket);
    }
    uint64_t t3 = get_time_ns();

    double sec_stream = (double)(t3 - t2) / (double)RBIPC_NSEC_PER_SEC;
    double mb_stream = ((double)iterations * payload_size) / (1024.0 * 1024.0 * sec_stream);

    out->payload_size      = payload_size;
    out->std_mb_per_sec    = mb_std;
    out->stream_mb_per_sec = mb_stream;
    out->speedup_pct       = ((mb_stream - mb_std) / mb_std) * 100.0;

    free(src_buffer);
    rbipc_detach(ring);
    rbipc_destroy(DEEP_SHM_A);
}

/* ============================================================================
 * 3. 1-to-N Multicast Publish-Subscribe Fanout Engine
 * ============================================================================ */
typedef struct {
    uint32_t num_subscribers;
    double msgs_per_sec;
    double mb_per_sec;
    double effective_delivery_rate;
} pubsub_fanout_bench_t;

typedef struct {
    rbipc_sub_t *sub;
    int expected_msgs;
} sub_thread_arg_t;

static void *pubsub_subscriber_worker(void *arg) {
    sub_thread_arg_t *a = (sub_thread_arg_t *)arg;
    uint64_t last_seq = 0;
    while (last_seq < (uint64_t)a->expected_msgs) {
        const void *msg = NULL;
        uint32_t len = 0;
        uint64_t seq = 0;
        int rc = rbipc_sub_read(a->sub, 200000000ULL, &msg, &len, &seq);
        if (rc == RBIPC_OK) {
            last_seq = seq + 1;
        } else if (rc == RBIPC_ERR_SHUTDOWN || rc == RBIPC_ERR_TIMEOUT) {
            break;
        }
    }
    return NULL;
}

static void run_pubsub_fanout_bench(uint32_t num_subs, int iterations, pubsub_fanout_bench_t *out) {
    rbipc_destroy(DEEP_PUBSUB);
    rbipc_ring_t *ring = NULL;
    assert(rbipc_create(DEEP_PUBSUB, 4096, 64, &ring) == RBIPC_OK);

    rbipc_sub_t *subs[8];
    for (uint32_t i = 0; i < num_subs; ++i) {
        assert(rbipc_sub_create(ring, &subs[i]) == RBIPC_OK);
    }

    pthread_t threads[8];
    sub_thread_arg_t args[8];
    for (uint32_t i = 0; i < num_subs; ++i) {
        args[i].sub = subs[i];
        args[i].expected_msgs = iterations;
        pthread_create(&threads[i], NULL, pubsub_subscriber_worker, &args[i]);
    }

    uint64_t t0 = get_time_ns();
    for (int i = 0; i < iterations; ++i) {
        uint64_t payload = (uint64_t)i;
        int rc = rbipc_publish(ring, &payload, sizeof(payload));
        assert(rc == RBIPC_OK);
    }

    for (uint32_t i = 0; i < num_subs; ++i) {
        pthread_join(threads[i], NULL);
        rbipc_sub_destroy(subs[i]);
    }
    uint64_t t1 = get_time_ns();

    double sec = (double)(t1 - t0) / (double)RBIPC_NSEC_PER_SEC;
    out->num_subscribers         = num_subs;
    out->msgs_per_sec            = (double)iterations / sec;
    out->mb_per_sec              = ((double)iterations * sizeof(uint64_t)) / (1024.0 * 1024.0 * sec);
    out->effective_delivery_rate = ((double)iterations * num_subs) / sec;

    rbipc_detach(ring);
    rbipc_destroy(DEEP_PUBSUB);
}

/* ============================================================================
 * 4. Reactor / Eventfd Notification vs Direct Spin
 * ============================================================================ */
typedef struct {
    double events_per_sec;
    double avg_latency_ns;
} reactor_bench_result_t;

static void run_reactor_bench(int iterations, reactor_bench_result_t *out) {
    rbipc_destroy(DEEP_SHM_A);
    rbipc_ring_t *ring = NULL;
    assert(rbipc_create(DEEP_SHM_A, 256, 64, &ring) == RBIPC_OK);

    int efd = -1;
    assert(rbipc_enable_eventfd(ring, &efd) == RBIPC_OK);

    int epfd = epoll_create1(0);
    assert(epfd >= 0);

    struct epoll_event ev = { .events = EPOLLIN, .data.fd = efd };
    assert(epoll_ctl(epfd, EPOLL_CTL_ADD, efd, &ev) == 0);

    uint64_t t0 = get_time_ns();
    for (int i = 0; i < iterations; ++i) {
        void *wbuf = NULL;
        uint64_t wticket = 0;
        assert(rbipc_reserve_write(ring, 32, &wbuf, &wticket) == RBIPC_OK);
        assert(rbipc_commit_write(ring, wticket, 32) == RBIPC_OK);

        struct epoll_event events[1];
        int nfds = epoll_wait(epfd, events, 1, 1000);
        assert(nfds == 1);

        uint64_t val = 0;
        assert(read(efd, &val, sizeof(val)) == (ssize_t)sizeof(val));

        const void *rbuf = NULL;
        uint32_t rlen = 0;
        uint64_t rticket = 0;
        assert(rbipc_read_acquire(ring, &rbuf, &rlen, &rticket) == RBIPC_OK);
        rbipc_read_release(ring, rticket);
    }
    uint64_t t1 = get_time_ns();

    close(epfd);
    rbipc_detach(ring);
    rbipc_destroy(DEEP_SHM_A);

    double sec = (double)(t1 - t0) / (double)RBIPC_NSEC_PER_SEC;
    out->events_per_sec = (double)iterations / sec;
    out->avg_latency_ns = (sec * (double)RBIPC_NSEC_PER_SEC) / (double)iterations;
}


/* ============================================================================
 * 5. SIMD (AVX2) Vectorized Sequence Scanning vs Scalar Loop
 * ============================================================================ */
#if defined(__AVX2__)
#include <immintrin.h>
#endif

typedef struct {
    double scalar_ns_per_batch;
    double simd_ns_per_batch;
    double speedup_pct;
} simd_scan_bench_t;

static void run_simd_scan_bench(int iterations, simd_scan_bench_t *out) {
    rbipc_destroy(DEEP_SHM_A);
    rbipc_ring_t *ring = NULL;
    assert(rbipc_create(DEEP_SHM_A, 1024, 64, &ring) == RBIPC_OK);

    uint32_t mask = ring->hdr->capacity_mask;
    rbipc_slot_t *slots = (rbipc_slot_t *)ring->slots;

    /* Populate slots as ready for reading */
    for (uint32_t i = 0; i < 1024; ++i) {
        atomic_store_explicit(&slots[i].sequence, i + 1, memory_order_relaxed);
    }

    uint32_t batch_size = 32;

    /* 1. Scalar scan measurement */
    uint64_t t0 = get_time_ns();
    uint64_t scalar_total = 0;
    for (int it = 0; it < iterations; ++it) {
        uint64_t t = (uint64_t)(it % 512);
        uint32_t avail = 0;
        for (uint32_t i = 0; i < batch_size; ++i) {
            uint64_t seq = atomic_load_explicit(&slots[(t + i) & mask].sequence, memory_order_acquire);
            if (seq == t + i + 1) {
                avail++;
            } else {
                break;
            }
        }
        scalar_total += avail;
    }
    uint64_t t1 = get_time_ns();

    /* 2. Vectorized AVX2 scan measurement */
    uint64_t t2 = get_time_ns();
    uint64_t simd_total = 0;
    for (int it = 0; it < iterations; ++it) {
        uint64_t t = (uint64_t)(it % 512);
        uint32_t avail = 0;
#if defined(__AVX2__)
        while (avail + 4 <= batch_size) {
            uint64_t base_t = t + avail;
            uint32_t idx = (uint32_t)(base_t & mask);
            uint64_t s0 = atomic_load_explicit(&slots[(idx + 0) & mask].sequence, memory_order_acquire);
            uint64_t s1 = atomic_load_explicit(&slots[(idx + 1) & mask].sequence, memory_order_acquire);
            uint64_t s2 = atomic_load_explicit(&slots[(idx + 2) & mask].sequence, memory_order_acquire);
            uint64_t s3 = atomic_load_explicit(&slots[(idx + 3) & mask].sequence, memory_order_acquire);

            __m256i vseq = _mm256_set_epi64x((long long)s3, (long long)s2, (long long)s1, (long long)s0);
            __m256i vtgt = _mm256_set_epi64x((long long)(base_t + 4), (long long)(base_t + 3),
                                             (long long)(base_t + 2), (long long)(base_t + 1));
            __m256i cmp = _mm256_cmpeq_epi64(vseq, vtgt);
            int match = _mm256_movemask_pd((__m256d)cmp);
            if (match == 0x0F) {
                avail += 4;
            } else {
                if ((match & 0x01) == 0) break;
                if ((match & 0x02) == 0) { avail += 1; break; }
                if ((match & 0x04) == 0) { avail += 2; break; }
                avail += 3;
                break;
            }
        }
#endif
        while (avail < batch_size) {
            uint64_t seq = atomic_load_explicit(&slots[(t + avail) & mask].sequence, memory_order_acquire);
            if (seq == t + avail + 1) {
                avail++;
            } else {
                break;
            }
        }
        simd_total += avail;
    }
    uint64_t t3 = get_time_ns();

    assert(scalar_total == simd_total);

    double scalar_ns = (double)(t1 - t0) / (double)iterations;
    double simd_ns   = (double)(t3 - t2) / (double)iterations;

    out->scalar_ns_per_batch = scalar_ns;
    out->simd_ns_per_batch   = simd_ns;
    out->speedup_pct         = ((scalar_ns - simd_ns) / scalar_ns) * 100.0;

    rbipc_detach(ring);
    rbipc_destroy(DEEP_SHM_A);
}

/* ============================================================================
 * 6. Consumer Lease Tracking Hot-Path Overhead Audit
 * ============================================================================ */
typedef struct {
    double no_lease_msgs_per_sec;
    double with_lease_msgs_per_sec;
    double overhead_pct;
} lease_bench_result_t;

static void run_lease_overhead_bench(int iterations, lease_bench_result_t *out) {
    rbipc_destroy(DEEP_SHM_A);
    rbipc_ring_t *ring = NULL;
    assert(rbipc_create(DEEP_SHM_A, 1024, 64, &ring) == RBIPC_OK);

    /* 1. Without Lease (default lease_timeout_ns == 0) */
    uint64_t t0 = get_time_ns();
    for (int i = 0; i < iterations; ++i) {
        void *wbuf = NULL;
        uint64_t wticket = 0;
        assert(rbipc_reserve_write(ring, 32, &wbuf, &wticket) == RBIPC_OK);
        assert(rbipc_commit_write(ring, wticket, 32) == RBIPC_OK);

        const void *rbuf = NULL;
        uint32_t rlen = 0;
        uint64_t rticket = 0;
        assert(rbipc_read_acquire(ring, &rbuf, &rlen, &rticket) == RBIPC_OK);
        rbipc_read_release(ring, rticket);
    }
    uint64_t t1 = get_time_ns();

    /* 2. With Lease enabled (e.g. 50ms deadline) */
    assert(rbipc_set_lease_timeout(ring, 50000000ULL) == RBIPC_OK);
    uint64_t t2 = get_time_ns();
    for (int i = 0; i < iterations; ++i) {
        void *wbuf = NULL;
        uint64_t wticket = 0;
        assert(rbipc_reserve_write(ring, 32, &wbuf, &wticket) == RBIPC_OK);
        assert(rbipc_commit_write(ring, wticket, 32) == RBIPC_OK);

        const void *rbuf = NULL;
        uint32_t rlen = 0;
        uint64_t rticket = 0;
        assert(rbipc_read_acquire(ring, &rbuf, &rlen, &rticket) == RBIPC_OK);
        rbipc_read_release(ring, rticket);
    }
    uint64_t t3 = get_time_ns();

    rbipc_detach(ring);
    rbipc_destroy(DEEP_SHM_A);

    double sec_no = (double)(t1 - t0) / (double)RBIPC_NSEC_PER_SEC;
    double sec_with = (double)(t3 - t2) / (double)RBIPC_NSEC_PER_SEC;

    out->no_lease_msgs_per_sec   = (double)iterations / sec_no;
    out->with_lease_msgs_per_sec = (double)iterations / sec_with;
    out->overhead_pct            = ((sec_with - sec_no) / sec_no) * 100.0;
}

/* ============================================================================
 * Main Benchmark Driver & JSON Report Generator
 * ============================================================================ */
int main(void) {
    printf("================================================================================\n");
    printf("     LIBRBIPC PHASE 1-5 DEEP DIVE ARCHITECTURAL BENCHMARK SUITE\n");
    printf("================================================================================\n");

    printf("\n--> [Benchmark 1/4] Adaptive Backoff (RBIPC_WAIT_ADAPTIVE) vs Zero-Spin (RBIPC_WAIT_ZERO_SPIN)\n");
    backoff_bench_result_t zero_spin_res;
    run_backoff_comparison(RBIPC_WAIT_ZERO_SPIN, &zero_spin_res);
    printf("    RBIPC_WAIT_ZERO_SPIN : %10.2f msgs/s | p50: %5u ns | p90: %5u ns | p99: %5u ns | max: %u ns\n",
           zero_spin_res.msgs_per_sec, zero_spin_res.p50_ns, zero_spin_res.p90_ns, zero_spin_res.p99_ns, zero_spin_res.max_ns);

    backoff_bench_result_t adaptive_res;
    run_backoff_comparison(RBIPC_WAIT_ADAPTIVE, &adaptive_res);
    printf("    RBIPC_WAIT_ADAPTIVE  : %10.2f msgs/s | p50: %5u ns | p90: %5u ns | p99: %5u ns | max: %u ns\n",
           adaptive_res.msgs_per_sec, adaptive_res.p50_ns, adaptive_res.p90_ns, adaptive_res.p99_ns, adaptive_res.max_ns);

    printf("\n--> [Benchmark 2/4] Non-Temporal Streaming Stores (rbipc_write_stream) vs Standard Cached memcpy\n");
    uint32_t stream_payloads[] = { 65536, 262144, 1048576, 4194304 };
    size_t num_sp = sizeof(stream_payloads) / sizeof(stream_payloads[0]);
    stream_store_bench_t stream_results[4];
    for (size_t i = 0; i < num_sp; ++i) {
        run_stream_store_bench(stream_payloads[i], 1000, &stream_results[i]);
        printf("    Payload %7u B: Std Cached: %8.2f MB/s | Non-Temporal: %8.2f MB/s | Delta: %+6.2f%%\n",
               stream_results[i].payload_size, stream_results[i].std_mb_per_sec,
               stream_results[i].stream_mb_per_sec, stream_results[i].speedup_pct);
    }

    printf("\n--> [Benchmark 3/4] 1-to-N Multicast PubSub Engine Fanout (50,000 published msgs)\n");
    uint32_t fanouts[] = { 1, 2, 4 };
    pubsub_fanout_bench_t pubsub_results[3];
    for (size_t i = 0; i < 3; ++i) {
        run_pubsub_fanout_bench(fanouts[i], 50000, &pubsub_results[i]);
        printf("    Fanout 1 -> %u Subs : Publish: %10.2f msgs/s (%6.2f MB/s) | Effective Delivered: %10.2f msgs/s\n",
               pubsub_results[i].num_subscribers, pubsub_results[i].msgs_per_sec,
               pubsub_results[i].mb_per_sec, pubsub_results[i].effective_delivery_rate);
    }

    printf("\n--> [Benchmark 4/4] Eventfd Reactor Asynchronous epoll Notification Latency (50,000 events)\n");
    reactor_bench_result_t reactor_res;
    run_reactor_bench(50000, &reactor_res);
    printf("    Reactor Throughput   : %10.2f events/s | Average Wake Latency: %.2f ns\n",
           reactor_res.events_per_sec, reactor_res.avg_latency_ns);

    printf("\n--> [Benchmark 5/6] SIMD (AVX2) Sequence Scanning vs Scalar Iteration (Batch Size 32, 500k scans)\n");
    simd_scan_bench_t simd_res;
    run_simd_scan_bench(500000, &simd_res);
    printf("    Scalar Loop Scan     : %.2f ns/batch\n", simd_res.scalar_ns_per_batch);
    printf("    AVX2 Vectorized Scan : %.2f ns/batch | Speedup: %+6.2f%%\n",
           simd_res.simd_ns_per_batch, simd_res.speedup_pct);

    printf("\n--> [Benchmark 6/6] Consumer Lease Tracking Hot-Path Overhead (500,000 round-trips)\n");
    lease_bench_result_t lease_res;
    run_lease_overhead_bench(500000, &lease_res);
    printf("    Without Lease Check  : %10.2f msgs/s\n", lease_res.no_lease_msgs_per_sec);
    printf("    With 50ms Lease SLA  : %10.2f msgs/s | Net Overhead: %5.2f%%\n",
           lease_res.with_lease_msgs_per_sec, lease_res.overhead_pct);

    /* Export JSON */
    char json_path[256];
    bench_get_result_path(json_path, sizeof(json_path), "deep_dive_features.json");
    FILE *fp = fopen(json_path, "w");
    if (fp) {
        fprintf(fp, "{\n");
        fprintf(fp, "  \"adaptive_vs_zerospin\": {\n");
        fprintf(fp, "    \"zerospin\": {\"msgs_per_sec\": %.2f, \"p50_ns\": %u, \"p90_ns\": %u, \"p99_ns\": %u, \"max_ns\": %u},\n",
                zero_spin_res.msgs_per_sec, zero_spin_res.p50_ns, zero_spin_res.p90_ns, zero_spin_res.p99_ns, zero_spin_res.max_ns);
        fprintf(fp, "    \"adaptive\": {\"msgs_per_sec\": %.2f, \"p50_ns\": %u, \"p90_ns\": %u, \"p99_ns\": %u, \"max_ns\": %u}\n",
                adaptive_res.msgs_per_sec, adaptive_res.p50_ns, adaptive_res.p90_ns, adaptive_res.p99_ns, adaptive_res.max_ns);
        fprintf(fp, "  },\n");
        fprintf(fp, "  \"streaming_stores\": [\n");
        for (size_t i = 0; i < num_sp; ++i) {
            fprintf(fp, "    {\"size\": %u, \"std_mb_s\": %.2f, \"stream_mb_s\": %.2f, \"speedup_pct\": %.2f}%s\n",
                    stream_results[i].payload_size, stream_results[i].std_mb_per_sec,
                    stream_results[i].stream_mb_per_sec, stream_results[i].speedup_pct,
                    (i + 1 < num_sp) ? "," : "");
        }
        fprintf(fp, "  ],\n");
        fprintf(fp, "  \"pubsub\": [\n");
        for (size_t i = 0; i < 3; ++i) {
            fprintf(fp, "    {\"subs\": %u, \"publish_msgs_s\": %.2f, \"publish_mb_s\": %.2f, \"delivered_msgs_s\": %.2f}%s\n",
                    pubsub_results[i].num_subscribers, pubsub_results[i].msgs_per_sec,
                    pubsub_results[i].mb_per_sec, pubsub_results[i].effective_delivery_rate,
                    (i + 1 < 3) ? "," : "");
        }
        fprintf(fp, "  ],\n");
        fprintf(fp, "  \"reactor\": {\"events_per_sec\": %.2f, \"avg_latency_ns\": %.2f},\n",
                reactor_res.events_per_sec, reactor_res.avg_latency_ns);
        fprintf(fp, "  \"simd_scanning\": {\"scalar_ns\": %.2f, \"simd_ns\": %.2f, \"speedup_pct\": %.2f},\n",
                simd_res.scalar_ns_per_batch, simd_res.simd_ns_per_batch, simd_res.speedup_pct);
        fprintf(fp, "  \"lease_overhead\": {\"no_lease_msgs_s\": %.2f, \"with_lease_msgs_s\": %.2f, \"overhead_pct\": %.2f}\n",
                lease_res.no_lease_msgs_per_sec, lease_res.with_lease_msgs_per_sec, lease_res.overhead_pct);
        fprintf(fp, "}\n");
        fclose(fp);
        printf("\nExported deep dive metrics to %s\n", json_path);
    }

    printf("================================================================================\n");
    printf("     DEEP DIVE BENCHMARKS COMPLETE\n");
    printf("================================================================================\n");
    return 0;
}
