/**
 * @file bench_suite.c
 * @brief Comprehensive, exhaustive benchmark suite for librbipc
 *
 * Measures:
 *  1. Idle / Backpressure CPU Utilization & Context Switches (Active vs Passive waiting)
 *  2. Half-Round-Trip Ping-Pong Latency Distribution (Min, Mean, p50, p90, p99, p99.9, p99.99, Max)
 *  3. Streaming Multi-Process IPC Throughput & Context Switch Audit
 *  4. B-Queue Batch Scaling (Batch Sizes 1, 4, 8, 16, 32, 64, 128)
 *  5. Variable Payload Size Throughput (64B, 256B, 1KB, 4KB, 16KB, 64KB, 256KB, 1MB, 4MB, 16MB)
 *  6. MPMC Concurrent Thread Scalability (1P-1C, 2P-2C, 4P-2C, 4P-4C)
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "rbipc.h"
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

static inline uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * RBIPC_NSEC_PER_SEC) + (uint64_t)ts.tv_nsec;
}

static int compare_uint32(const void *a, const void *b) {
    uint32_t val_a = *(const uint32_t *)a;
    uint32_t val_b = *(const uint32_t *)b;
    if (val_a < val_b) return -1;
    if (val_a > val_b) return 1;
    return 0;
}

/* ============================================================================
 * Benchmark 1: Idle / Backpressure Waiting CPU Utilization & Context Switches
 * ============================================================================ */
typedef struct {
    double user_sec;
    double sys_sec;
    double total_sec;
    double cpu_pct;
    long voluntary_ctxt_switches;
    long involuntary_ctxt_switches;
} idle_metrics_t;

static void run_bench_idle(idle_metrics_t *out_metrics) {
    printf("\n[1/6] Running Idle / Backpressure Waiting CPU Test (2.0s observation)...\n");
    rbipc_destroy(BENCH_SHM_A);

    rbipc_ring_t *ring = NULL;
    int rc = rbipc_create(BENCH_SHM_A, 256, 64, &ring);
    assert(rc == RBIPC_OK);

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        perror("pipe");
        exit(1);
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(1);
    }

    if (pid == 0) {
        /* Child: Consumer waiting passively on empty ring */
        close(pipefd[0]);
        rbipc_ring_t *c_ring = NULL;
        rc = rbipc_attach(BENCH_SHM_A, &c_ring);
        assert(rc == RBIPC_OK);

        const void *buf = NULL;
        uint32_t len = 0, ticket = 0;

        /* Will block until shutdown */
        rc = rbipc_read_acquire(c_ring, &buf, &len, &ticket);
        assert(rc == RBIPC_ERR_SHUTDOWN);

        struct rusage ru;
        getrusage(RUSAGE_SELF, &ru);

        idle_metrics_t m;
        m.user_sec = (double)ru.ru_utime.tv_sec + (double)ru.ru_utime.tv_usec / 1e6;
        m.sys_sec  = (double)ru.ru_stime.tv_sec + (double)ru.ru_stime.tv_usec / 1e6;
        m.total_sec = m.user_sec + m.sys_sec;
        m.voluntary_ctxt_switches = ru.ru_nvcsw;
        m.involuntary_ctxt_switches = ru.ru_nivcsw;

        write(pipefd[1], &m, sizeof(m));
        close(pipefd[1]);
        rbipc_detach(c_ring);
        _exit(0);
    }

    /* Parent: Sleep for 2.0 seconds while child waits */
    close(pipefd[1]);
    struct timespec req = { .tv_sec = 2, .tv_nsec = 0 };
    nanosleep(&req, NULL);

    rbipc_signal_shutdown(ring);

    idle_metrics_t m;
    read(pipefd[0], &m, sizeof(m));
    close(pipefd[0]);
    waitpid(pid, NULL, 0);

    rbipc_detach(ring);
    rbipc_destroy(BENCH_SHM_A);

    m.cpu_pct = (m.total_sec / 2.0) * 100.0;
    *out_metrics = m;

    printf("  User CPU Time   : %.4f s\n", m.user_sec);
    printf("  System CPU Time : %.4f s\n", m.sys_sec);
    printf("  Total CPU Used  : %.4f s (%.2f%% core usage)\n", m.total_sec, m.cpu_pct);
    printf("  Voluntary CS    : %ld\n", m.voluntary_ctxt_switches);
    printf("  Involuntary CS  : %ld\n", m.involuntary_ctxt_switches);
}

/* ============================================================================
 * Benchmark 2: Ping-Pong Round-Trip Latency Distribution
 * ============================================================================ */
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

static void run_bench_ping_pong(int iterations, latency_dist_t *out_dist) {
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
            uint32_t rlen = 0, rticket = 0;
            rc = rbipc_read_acquire(c_a, &rbuf, &rlen, &rticket);
            if (rc != RBIPC_OK) break;

            uint64_t ts = *(const uint64_t *)rbuf;
            rbipc_read_release(c_a, rticket);

            void *wbuf = NULL;
            uint32_t wticket = 0;
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
        uint32_t wticket = 0;

        assert(rbipc_reserve_write(ring_a, sizeof(uint64_t), &wbuf, &wticket) == RBIPC_OK);
        *(uint64_t *)wbuf = t0;
        assert(rbipc_commit_write(ring_a, wticket, sizeof(uint64_t)) == RBIPC_OK);

        const void *rbuf = NULL;
        uint32_t rlen = 0, rticket = 0;
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

/* ============================================================================
 * Benchmark 3: High-Throughput Streaming IPC (1P - 1C)
 * ============================================================================ */
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

static void run_bench_streaming(int iterations, stream_metrics_t *out_metrics) {
    printf("\n[3/6] Running High-Throughput Streaming IPC (%d messages, 64-byte payload)...\n", iterations);
    rbipc_destroy(BENCH_SHM_A);

    rbipc_ring_t *ring = NULL;
    assert(rbipc_create(BENCH_SHM_A, 2048, 128, &ring) == RBIPC_OK);

    int pipefd[2];
    assert(pipe(pipefd) == 0);

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(1);
    }

    if (pid == 0) {
        /* Producer Child */
        close(pipefd[0]);
        rbipc_ring_t *p_ring = NULL;
        assert(rbipc_attach(BENCH_SHM_A, &p_ring) == RBIPC_OK);

        char payload[64];
        memset(payload, 0x42, sizeof(payload));

        for (int i = 0; i < iterations; ++i) {
            void *buf = NULL;
            uint32_t ticket = 0;
            int rc = rbipc_reserve_write(p_ring, sizeof(payload), &buf, &ticket);
            assert(rc == RBIPC_OK);
            *(uint32_t *)buf = (uint32_t)i;
            rc = rbipc_commit_write(p_ring, ticket, sizeof(payload));
            assert(rc == RBIPC_OK);
        }

        rbipc_signal_shutdown(p_ring);

        struct rusage ru;
        getrusage(RUSAGE_SELF, &ru);
        long cs[2] = { ru.ru_nvcsw, ru.ru_nivcsw };
        write(pipefd[1], cs, sizeof(cs));
        close(pipefd[1]);

        rbipc_detach(p_ring);
        _exit(0);
    }

    /* Consumer Parent */
    close(pipefd[1]);
    uint64_t t0 = get_time_ns();
    int count = 0;

    while (count < iterations) {
        const void *buf = NULL;
        uint32_t len = 0, ticket = 0;
        int rc = rbipc_read_acquire(ring, &buf, &len, &ticket);
        if (rc == RBIPC_ERR_SHUTDOWN) break;
        assert(rc == RBIPC_OK);
        count++;
        rbipc_read_release(ring, ticket);
    }

    uint64_t t1 = get_time_ns();
    double elapsed = (double)(t1 - t0) / 1e9;

    long prod_cs[2];
    read(pipefd[0], prod_cs, sizeof(prod_cs));
    close(pipefd[0]);
    waitpid(pid, NULL, 0);

    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);

    out_metrics->iterations = count;
    out_metrics->elapsed_sec = elapsed;
    out_metrics->msgs_per_sec = (double)count / elapsed;
    out_metrics->mb_per_sec = (out_metrics->msgs_per_sec * 64.0) / (1024.0 * 1024.0);
    out_metrics->avg_latency_ns = (elapsed * 1e9) / (double)count;
    out_metrics->producer_nvcsw = prod_cs[0];
    out_metrics->producer_nivcsw = prod_cs[1];
    out_metrics->consumer_nvcsw = ru.ru_nvcsw;
    out_metrics->consumer_nivcsw = ru.ru_nivcsw;

    rbipc_detach(ring);
    rbipc_destroy(BENCH_SHM_A);

    printf("  Messages Handled: %d in %.4f s\n", count, elapsed);
    printf("  Throughput      : %.2f msgs/sec (%.2f MB/sec)\n", out_metrics->msgs_per_sec, out_metrics->mb_per_sec);
    printf("  Average Latency : %.2f ns/msg\n", out_metrics->avg_latency_ns);
    printf("  Producer CS     : %ld vol, %ld invol\n", out_metrics->producer_nvcsw, out_metrics->producer_nivcsw);
    printf("  Consumer CS     : %ld vol, %ld invol\n", out_metrics->consumer_nvcsw, out_metrics->consumer_nivcsw);
}

/* ============================================================================
 * Benchmark 4: B-Queue Vector Batching Scaling
 * ============================================================================ */
typedef struct {
    uint32_t batch_size;
    double   msgs_per_sec;
    double   mb_per_sec;
    double   avg_latency_ns;
} batch_result_t;

static void run_bench_batching(uint32_t batch_size, int total_msgs, batch_result_t *out_res) {
    rbipc_destroy(BENCH_SHM_A);

    rbipc_ring_t *ring = NULL;
    assert(rbipc_create(BENCH_SHM_A, 2048, 128, &ring) == RBIPC_OK);

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(1);
    }

    if (pid == 0) {
        /* Producer Child */
        rbipc_ring_t *p_ring = NULL;
        assert(rbipc_attach(BENCH_SHM_A, &p_ring) == RBIPC_OK);

        rbipc_iovec_t iovecs[128];
        uint32_t tickets[128];
        uint32_t lens[128];

        int sent = 0;
        while (sent < total_msgs) {
            uint32_t req = (uint32_t)(total_msgs - sent);
            if (req > batch_size) req = batch_size;

            uint32_t reserved = 0;
            assert(rbipc_reserve_write_batch(p_ring, req, iovecs, &reserved) == RBIPC_OK);

            for (uint32_t i = 0; i < reserved; ++i) {
                *(uint32_t *)iovecs[i].buf = (uint32_t)(sent + i);
                tickets[i] = iovecs[i].ticket;
                lens[i] = 64;
            }

            assert(rbipc_commit_write_batch(p_ring, reserved, tickets, lens) == RBIPC_OK);
            sent += (int)reserved;
        }

        rbipc_signal_shutdown(p_ring);
        rbipc_detach(p_ring);
        _exit(0);
    }

    /* Consumer Parent */
    rbipc_rovec_t rovecs[128];
    uint32_t tickets[128];

    uint64_t t0 = get_time_ns();
    int count = 0;

    while (count < total_msgs) {
        uint32_t req = (uint32_t)(total_msgs - count);
        if (req > batch_size) req = batch_size;

        uint32_t acquired = 0;
        int rc = rbipc_read_acquire_batch(ring, req, rovecs, &acquired);
        if (rc == RBIPC_ERR_SHUTDOWN) break;
        assert(rc == RBIPC_OK);

        for (uint32_t i = 0; i < acquired; ++i) {
            tickets[i] = rovecs[i].ticket;
        }

        assert(rbipc_read_release_batch(ring, acquired, tickets) == RBIPC_OK);
        count += (int)acquired;
    }

    uint64_t t1 = get_time_ns();
    double elapsed = (double)(t1 - t0) / 1e9;
    waitpid(pid, NULL, 0);

    rbipc_detach(ring);
    rbipc_destroy(BENCH_SHM_A);

    out_res->batch_size = batch_size;
    out_res->msgs_per_sec = (double)count / elapsed;
    out_res->mb_per_sec = (out_res->msgs_per_sec * 64.0) / (1024.0 * 1024.0);
    out_res->avg_latency_ns = (elapsed * 1e9) / (double)count;

    printf("  Batch Size %3u: %10.2f msgs/sec | %8.2f MB/sec | %7.2f ns/msg\n",
           batch_size, out_res->msgs_per_sec, out_res->mb_per_sec, out_res->avg_latency_ns);
}

/* ============================================================================
 * Benchmark 5: Variable Payload Size Scaling
 * ============================================================================ */
typedef struct {
    uint32_t payload_size;
    double   msgs_per_sec;
    double   mb_per_sec;
} payload_result_t;

static void run_bench_payload(uint32_t payload_size, int count, payload_result_t *out_res) {
    rbipc_destroy(BENCH_SHM_A);

    rbipc_ring_t *ring = NULL;
    assert(rbipc_create(BENCH_SHM_A, 512, payload_size, &ring) == RBIPC_OK);

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(1);
    }

    if (pid == 0) {
        rbipc_ring_t *p_ring = NULL;
        assert(rbipc_attach(BENCH_SHM_A, &p_ring) == RBIPC_OK);

        for (int i = 0; i < count; ++i) {
            void *buf = NULL;
            uint32_t ticket = 0;
            assert(rbipc_reserve_write(p_ring, payload_size, &buf, &ticket) == RBIPC_OK);
            *(uint32_t *)buf = (uint32_t)i;
            assert(rbipc_commit_write(p_ring, ticket, payload_size) == RBIPC_OK);
        }

        rbipc_signal_shutdown(p_ring);
        rbipc_detach(p_ring);
        _exit(0);
    }

    uint64_t t0 = get_time_ns();
    int rec = 0;

    while (rec < count) {
        const void *buf = NULL;
        uint32_t len = 0, ticket = 0;
        int rc = rbipc_read_acquire(ring, &buf, &len, &ticket);
        if (rc == RBIPC_ERR_SHUTDOWN) break;
        assert(rc == RBIPC_OK);
        rec++;
        rbipc_read_release(ring, ticket);
    }

    uint64_t t1 = get_time_ns();
    double elapsed = (double)(t1 - t0) / 1e9;
    waitpid(pid, NULL, 0);

    rbipc_detach(ring);
    rbipc_destroy(BENCH_SHM_A);

    out_res->payload_size = payload_size;
    out_res->msgs_per_sec = (double)rec / elapsed;
    out_res->mb_per_sec = (out_res->msgs_per_sec * (double)payload_size) / (1024.0 * 1024.0);

    printf("  Payload %8u B: %10.2f msgs/sec | %8.2f MB/sec\n",
           payload_size, out_res->msgs_per_sec, out_res->mb_per_sec);
}

/* ============================================================================
 * Benchmark 6: Multi-Threaded MPMC Scalability
 * ============================================================================ */
typedef struct {
    uint32_t num_prod;
    uint32_t num_cons;
    double   msgs_per_sec;
    double   avg_latency_ns;
} mpmc_result_t;

typedef struct {
    rbipc_ring_t *ring;
    uint32_t count;
} thread_arg_t;

static _Atomic uint32_t g_mpmc_consumed;

static void *mpmc_prod_thread(void *arg) {
    thread_arg_t *targ = (thread_arg_t *)arg;
    rbipc_ring_t *ring = targ->ring;
    uint32_t count = targ->count;

    for (uint32_t i = 0; i < count; ++i) {
        void *buf = NULL;
        uint32_t ticket = 0;
        int rc = rbipc_reserve_write(ring, 64, &buf, &ticket);
        assert(rc == RBIPC_OK);
        *(uint32_t *)buf = i;
        rc = rbipc_commit_write(ring, ticket, 64);
        assert(rc == RBIPC_OK);
    }
    return NULL;
}

static void *mpmc_cons_thread(void *arg) {
    thread_arg_t *targ = (thread_arg_t *)arg;
    rbipc_ring_t *ring = targ->ring;
    uint32_t total = targ->count;

    while (atomic_load_explicit(&g_mpmc_consumed, memory_order_relaxed) < total) {
        const void *buf = NULL;
        uint32_t len = 0, ticket = 0;
        int rc = rbipc_read_acquire_timeout(ring, 50000000ULL, &buf, &len, &ticket);
        if (rc == RBIPC_ERR_TIMEOUT || rc == RBIPC_ERR_SHUTDOWN) {
            if (atomic_load_explicit(&g_mpmc_consumed, memory_order_relaxed) >= total) break;
            continue;
        }
        assert(rc == RBIPC_OK);
        atomic_fetch_add_explicit(&g_mpmc_consumed, 1, memory_order_relaxed);
        rbipc_read_release(ring, ticket);
    }
    return NULL;
}

static void run_bench_mpmc(uint32_t np, uint32_t nc, uint32_t total, mpmc_result_t *out_res) {
    atomic_init(&g_mpmc_consumed, 0);

    rbipc_ring_t *ring = NULL;
    assert(rbipc_create(NULL, 1024, 64, &ring) == RBIPC_OK);

    pthread_t prods[np];
    pthread_t cons[nc];
    thread_arg_t parg = { .ring = ring, .count = total / np };
    thread_arg_t carg = { .ring = ring, .count = total };

    uint64_t t0 = get_time_ns();

    for (uint32_t i = 0; i < nc; ++i) {
        pthread_create(&cons[i], NULL, mpmc_cons_thread, &carg);
    }
    for (uint32_t i = 0; i < np; ++i) {
        pthread_create(&prods[i], NULL, mpmc_prod_thread, &parg);
    }

    for (uint32_t i = 0; i < np; ++i) {
        pthread_join(prods[i], NULL);
    }
    for (uint32_t i = 0; i < nc; ++i) {
        pthread_join(cons[i], NULL);
    }

    uint64_t t1 = get_time_ns();
    double elapsed = (double)(t1 - t0) / 1e9;

    rbipc_detach(ring);

    out_res->num_prod = np;
    out_res->num_cons = nc;
    out_res->msgs_per_sec = (double)total / elapsed;
    out_res->avg_latency_ns = (elapsed * 1e9) / (double)total;

    printf("  MPMC (%uP - %uC): %10.2f msgs/sec | %7.2f ns/msg\n",
           np, nc, out_res->msgs_per_sec, out_res->avg_latency_ns);
}

/* ============================================================================
 * Main Driver: Runs all 6 benchmark categories and exports structured report
 * ============================================================================ */
int main(int argc, char **argv) {
    const char *tag = (argc > 1) ? argv[1] : "zero_spin_optimized";
    printf("================================================================================\n");
    printf("            LIBRBIPC EXHAUSTIVE BENCHMARK SUITE: [%s]\n", tag);
    printf("================================================================================\n");

    idle_metrics_t idle;
    run_bench_idle(&idle);

    latency_dist_t lat;
    run_bench_ping_pong(100000, &lat);

    stream_metrics_t stream;
    run_bench_streaming(1000000, &stream);

    printf("\n[4/6] Running B-Queue Vector Batching Scaling (1,000,000 msgs each)...\n");
    uint32_t batch_sizes[] = { 1, 4, 8, 16, 32, 64, 128 };
    size_t num_batches = sizeof(batch_sizes) / sizeof(batch_sizes[0]);
    batch_result_t batch_results[num_batches];
    for (size_t i = 0; i < num_batches; ++i) {
        run_bench_batching(batch_sizes[i], 1000000, &batch_results[i]);
    }

    printf("\n[5/6] Running Payload Size Scaling (200,000 msgs each)...\n");
    uint32_t payloads[] = { 64, 256, 1024, 4096, 16384, 65536, 262144, 1048576, 4194304, 16777216 };
    size_t num_payloads = sizeof(payloads) / sizeof(payloads[0]);
    payload_result_t payload_results[num_payloads];
    for (size_t i = 0; i < num_payloads; ++i) {
        run_bench_payload(payloads[i], 200000, &payload_results[i]);
    }

    printf("\n[6/6] Running MPMC Concurrent Contention Scalability (500,000 msgs each)...\n");
    mpmc_result_t mpmc_results[4];
    run_bench_mpmc(1, 1, 500000, &mpmc_results[0]);
    run_bench_mpmc(2, 2, 500000, &mpmc_results[1]);
    run_bench_mpmc(4, 2, 500000, &mpmc_results[2]);
    run_bench_mpmc(4, 4, 500000, &mpmc_results[3]);

    /* Write JSON results file */
    char json_filename[256];
    if (access("benchmarks/results", W_OK) == 0) {
        snprintf(json_filename, sizeof(json_filename), "benchmarks/results/%s.json", tag);
    } else {
        snprintf(json_filename, sizeof(json_filename), "bench_%s.json", tag);
    }
    FILE *fp = fopen(json_filename, "w");
    if (fp) {
        fprintf(fp, "{\n");
        fprintf(fp, "  \"tag\": \"%s\",\n", tag);
        fprintf(fp, "  \"idle\": {\n");
        fprintf(fp, "    \"user_sec\": %.6f,\n", idle.user_sec);
        fprintf(fp, "    \"sys_sec\": %.6f,\n", idle.sys_sec);
        fprintf(fp, "    \"total_sec\": %.6f,\n", idle.total_sec);
        fprintf(fp, "    \"cpu_pct\": %.2f,\n", idle.cpu_pct);
        fprintf(fp, "    \"vol_cs\": %ld,\n", idle.voluntary_ctxt_switches);
        fprintf(fp, "    \"invol_cs\": %ld\n", idle.involuntary_ctxt_switches);
        fprintf(fp, "  },\n");
        fprintf(fp, "  \"latency\": {\n");
        fprintf(fp, "    \"min_ns\": %u,\n", lat.min_ns);
        fprintf(fp, "    \"mean_ns\": %.2f,\n", lat.mean_ns);
        fprintf(fp, "    \"p50_ns\": %u,\n", lat.p50_ns);
        fprintf(fp, "    \"p90_ns\": %u,\n", lat.p90_ns);
        fprintf(fp, "    \"p99_ns\": %u,\n", lat.p99_ns);
        fprintf(fp, "    \"p99_9_ns\": %u,\n", lat.p99_9_ns);
        fprintf(fp, "    \"p99_99_ns\": %u,\n", lat.p99_99_ns);
        fprintf(fp, "    \"max_ns\": %u,\n", lat.max_ns);
        fprintf(fp, "    \"stddev_ns\": %.2f\n", lat.stddev_ns);
        fprintf(fp, "  },\n");
        fprintf(fp, "  \"streaming\": {\n");
        fprintf(fp, "    \"msgs_per_sec\": %.2f,\n", stream.msgs_per_sec);
        fprintf(fp, "    \"mb_per_sec\": %.2f,\n", stream.mb_per_sec);
        fprintf(fp, "    \"avg_latency_ns\": %.2f,\n", stream.avg_latency_ns);
        fprintf(fp, "    \"producer_vol_cs\": %ld,\n", stream.producer_nvcsw);
        fprintf(fp, "    \"producer_invol_cs\": %ld,\n", stream.producer_nivcsw);
        fprintf(fp, "    \"consumer_vol_cs\": %ld,\n", stream.consumer_nvcsw);
        fprintf(fp, "    \"consumer_invol_cs\": %ld\n", stream.consumer_nivcsw);
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
        for (size_t i = 0; i < 4; ++i) {
            fprintf(fp, "    {\"p\": %u, \"c\": %u, \"msgs_per_sec\": %.2f, \"avg_latency_ns\": %.2f}%s\n",
                    mpmc_results[i].num_prod, mpmc_results[i].num_cons,
                    mpmc_results[i].msgs_per_sec, mpmc_results[i].avg_latency_ns,
                    (i + 1 < 4) ? "," : "");
        }
        fprintf(fp, "  ]\n");
        fprintf(fp, "}\n");
        fclose(fp);
        printf("\nExported JSON benchmark metrics to %s\n", json_filename);
    }

    printf("================================================================================\n");
    printf("            BENCHMARK COMPLETE: [%s]\n", tag);
    printf("================================================================================\n");
    return 0;
}
