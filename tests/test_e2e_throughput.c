/**
 * @file test_e2e_throughput.c
 * @brief High-throughput & low-latency multi-process fork benchmark
 */

#include "rbipc.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

#define BENCH_SHM_NAME   "/rbipc_bench_shm"
#define BENCH_CAPACITY   1024
#define BENCH_SLOT_SIZE  256
#define BENCH_ITERATIONS 200000

typedef struct {
    uint64_t seq_id;
    uint64_t timestamp_ns;
    char payload[128];
} bench_msg_t;

static inline uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * RBIPC_NSEC_PER_SEC) + (uint64_t)ts.tv_nsec;
}

static void run_producer(int iterations) {
    rbipc_ring_t *ring = NULL;
    int rc = rbipc_attach(BENCH_SHM_NAME, &ring);
    if (rc != RBIPC_OK) {
        fprintf(stderr, "[Producer] Failed to attach: %d\n", rc);
        exit(1);
    }

    for (int i = 0; i < iterations; ++i) {
        void *buf = NULL;
        uint32_t ticket = 0;

        rc = rbipc_reserve_write(ring, sizeof(bench_msg_t), &buf, &ticket);
        if (rc != RBIPC_OK) {
            fprintf(stderr, "[Producer] Reservation failed at %d: %d\n", i, rc);
            exit(1);
        }

        bench_msg_t *msg = (bench_msg_t *)buf;
        msg->seq_id = (uint64_t)i;
        msg->timestamp_ns = get_time_ns();
        snprintf(msg->payload, sizeof(msg->payload), "BENCH_MSG_%08d", i);

        rc = rbipc_commit_write(ring, ticket, sizeof(bench_msg_t));
        if (rc != RBIPC_OK) {
            fprintf(stderr, "[Producer] Commit failed at %d: %d\n", i, rc);
            exit(1);
        }
    }

    rbipc_signal_shutdown(ring);
    rbipc_detach(ring);
    exit(0);
}

static void run_consumer(int expected_iterations) {
    rbipc_ring_t *ring = NULL;
    int rc = rbipc_attach(BENCH_SHM_NAME, &ring);
    if (rc != RBIPC_OK) {
        fprintf(stderr, "[Consumer] Failed to attach: %d\n", rc);
        exit(1);
    }

    uint64_t start_time = get_time_ns();
    int received_count = 0;
    uint64_t expected_seq = 0;

    while (received_count < expected_iterations) {
        const void *buf = NULL;
        uint32_t len = 0;
        uint32_t ticket = 0;

        rc = rbipc_read_acquire(ring, &buf, &len, &ticket);
        if (rc == RBIPC_ERR_SHUTDOWN) {
            break;
        } else if (rc != RBIPC_OK) {
            fprintf(stderr, "[Consumer] Acquire error: %d\n", rc);
            break;
        }

        assert(len == sizeof(bench_msg_t));
        const bench_msg_t *msg = (const bench_msg_t *)buf;
        assert(msg->seq_id == expected_seq);

        expected_seq++;
        received_count++;

        rbipc_read_release(ring, ticket);
    }

    uint64_t elapsed_ns = get_time_ns() - start_time;
    double elapsed_sec = (double)elapsed_ns / 1e9;
    double throughput = (double)received_count / elapsed_sec;
    double avg_latency_ns = (double)elapsed_ns / (double)received_count;

    printf("\n=== librbipc Throughput Benchmark ===\n");
    printf("Total Messages : %d\n", received_count);
    printf("Elapsed Time   : %.4f seconds\n", elapsed_sec);
    printf("Throughput     : %.2f msgs/sec (%.2f MB/sec)\n",
           throughput, (throughput * sizeof(bench_msg_t)) / (1024.0 * 1024.0));
    printf("Average Latency: %.2f ns/message\n\n", avg_latency_ns);

    rbipc_detach(ring);
}

static void run_producer_batch(int iterations, uint32_t batch_size) {
    rbipc_ring_t *ring = NULL;
    int rc = rbipc_attach(BENCH_SHM_NAME, &ring);
    if (rc != RBIPC_OK) {
        fprintf(stderr, "[Producer Batch] Failed to attach: %d\n", rc);
        exit(1);
    }

    rbipc_iovec_t iovecs[64];
    uint32_t tickets[64];
    uint32_t lens[64];

    int sent = 0;
    while (sent < iterations) {
        uint32_t req = (uint32_t)(iterations - sent);
        if (req > batch_size) req = batch_size;

        uint32_t reserved = 0;
        rc = rbipc_reserve_write_batch(ring, req, iovecs, &reserved);
        if (rc != RBIPC_OK) {
            fprintf(stderr, "[Producer Batch] Reservation failed: %d\n", rc);
            exit(1);
        }

        for (uint32_t i = 0; i < reserved; ++i) {
            bench_msg_t *msg = (bench_msg_t *)iovecs[i].buf;
            msg->seq_id = (uint64_t)(sent + i);
            msg->timestamp_ns = 0;
            tickets[i] = iovecs[i].ticket;
            lens[i] = sizeof(bench_msg_t);
        }

        rc = rbipc_commit_write_batch(ring, reserved, tickets, lens);
        if (rc != RBIPC_OK) {
            fprintf(stderr, "[Producer Batch] Commit failed: %d\n", rc);
            exit(1);
        }

        sent += (int)reserved;
    }

    rbipc_signal_shutdown(ring);
    rbipc_detach(ring);
    exit(0);
}

static void run_consumer_batch(int expected_iterations, uint32_t batch_size) {
    rbipc_ring_t *ring = NULL;
    int rc = rbipc_attach(BENCH_SHM_NAME, &ring);
    if (rc != RBIPC_OK) {
        fprintf(stderr, "[Consumer Batch] Failed to attach: %d\n", rc);
        exit(1);
    }

    uint64_t start_time = get_time_ns();
    int received_count = 0;
    uint64_t expected_seq = 0;
    rbipc_rovec_t rovecs[64];
    uint32_t tickets[64];

    while (received_count < expected_iterations) {
        uint32_t req = (uint32_t)(expected_iterations - received_count);
        if (req > batch_size) req = batch_size;

        uint32_t acquired = 0;
        rc = rbipc_read_acquire_batch(ring, req, rovecs, &acquired);
        if (rc == RBIPC_ERR_SHUTDOWN) {
            break;
        } else if (rc != RBIPC_OK) {
            fprintf(stderr, "[Consumer Batch] Acquire error: %d\n", rc);
            break;
        }

        for (uint32_t i = 0; i < acquired; ++i) {
            assert(rovecs[i].len == sizeof(bench_msg_t));
            const bench_msg_t *msg = (const bench_msg_t *)rovecs[i].buf;
            assert(msg->seq_id == expected_seq);
            expected_seq++;
            tickets[i] = rovecs[i].ticket;
        }

        received_count += (int)acquired;
        rbipc_read_release_batch(ring, acquired, tickets);
    }

    uint64_t elapsed_ns = get_time_ns() - start_time;
    double elapsed_sec = (double)elapsed_ns / 1e9;
    double throughput = (double)received_count / elapsed_sec;
    double avg_latency_ns = (double)elapsed_ns / (double)received_count;

    printf("\n=== librbipc B-Queue Batch Benchmark (Batch Size: %u) ===\n", batch_size);
    printf("Total Messages : %d\n", received_count);
    printf("Elapsed Time   : %.4f seconds\n", elapsed_sec);
    printf("Throughput     : %.2f msgs/sec (%.2f MB/sec)\n",
           throughput, (throughput * sizeof(bench_msg_t)) / (1024.0 * 1024.0));
    printf("Average Latency: %.2f ns/message\n\n", avg_latency_ns);

    rbipc_detach(ring);
}

int main(void) {
    /* 1. Single-message benchmark */
    rbipc_destroy(BENCH_SHM_NAME);

    rbipc_ring_t *creator = NULL;
    int rc = rbipc_create(BENCH_SHM_NAME, BENCH_CAPACITY, BENCH_SLOT_SIZE, &creator);
    assert(rc == RBIPC_OK);

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    } else if (pid == 0) {
        run_producer(BENCH_ITERATIONS);
    }

    run_consumer(BENCH_ITERATIONS);
    waitpid(pid, NULL, 0);

    rbipc_detach(creator);
    rbipc_destroy(BENCH_SHM_NAME);

    /* 2. B-Queue Batch benchmark (Batch size 32, 500k messages) */
    rc = rbipc_create(BENCH_SHM_NAME, BENCH_CAPACITY, BENCH_SLOT_SIZE, &creator);
    assert(rc == RBIPC_OK);

    const int batch_iterations = 500000;
    pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    } else if (pid == 0) {
        run_producer_batch(batch_iterations, 32);
    }

    run_consumer_batch(batch_iterations, 32);
    waitpid(pid, NULL, 0);

    rbipc_detach(creator);
    rbipc_destroy(BENCH_SHM_NAME);
    return 0;
}
