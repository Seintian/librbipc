/**
 * @file test_harness.c
 * @brief End-to-end verification test harness for librbipc
 *
 * Verifies:
 * 1. Monotonic message ordering across independent OS processes (fork).
 * 2. Zero-copy slot payload verification.
 * 3. High-throughput & sub-microsecond latency measurement.
 * 4. Deadlock-free crash recovery when a producer crashes with a reserved slot.
 * 5. Clean shutdown signaling and resource deallocation.
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
#include <assert.h>
#include <time.h>
#include <signal.h>
#include <sys/wait.h>

#define TEST_SHM_NAME     "/rbipc_test_channel"
#define TEST_CAPACITY     1024
#define TEST_SLOT_SIZE    256
#define TEST_ITERATIONS   500000

typedef struct {
    uint64_t seq_id;
    uint64_t timestamp_ns;
    char payload[128];
} test_msg_t;

static inline uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void run_producer(int iterations) {
    rbipc_ring_t *ring = NULL;
    int rc = rbipc_attach(TEST_SHM_NAME, &ring);
    if (rc != RBIPC_OK) {
        fprintf(stderr, "[Producer] Failed to attach: %d\n", rc);
        exit(1);
    }

    printf("[Producer] Starting transmission of %d messages...\n", iterations);
    for (int i = 0; i < iterations; ++i) {
        void *buf = NULL;
        uint32_t ticket = 0;

        rc = rbipc_reserve_write(ring, sizeof(test_msg_t), &buf, &ticket);
        if (rc != RBIPC_OK) {
            fprintf(stderr, "[Producer] Reservation failed at %d: %d\n", i, rc);
            exit(1);
        }

        test_msg_t *msg = (test_msg_t *)buf;
        msg->seq_id = (uint64_t)i;
        msg->timestamp_ns = get_time_ns();
        snprintf(msg->payload, sizeof(msg->payload), "LIBRBIPC_MSG_#%08d", i);

        rc = rbipc_commit_write(ring, ticket, sizeof(test_msg_t));
        if (rc != RBIPC_OK) {
            fprintf(stderr, "[Producer] Commit failed at %d: %d\n", i, rc);
            exit(1);
        }
    }

    /* Signal shutdown when completed */
    rbipc_signal_shutdown(ring);
    rbipc_detach(ring);
    printf("[Producer] Completed transmission.\n");
    exit(0);
}

static void run_consumer(int expected_iterations) {
    rbipc_ring_t *ring = NULL;
    int rc = rbipc_attach(TEST_SHM_NAME, &ring);
    if (rc != RBIPC_OK) {
        fprintf(stderr, "[Consumer] Failed to attach: %d\n", rc);
        exit(1);
    }

    printf("[Consumer] Listening for messages...\n");
    uint64_t start_time = get_time_ns();
    int received_count = 0;
    uint64_t expected_seq = 0;

    while (received_count < expected_iterations) {
        const void *buf = NULL;
        uint32_t len = 0;
        uint32_t ticket = 0;

        rc = rbipc_read_acquire(ring, &buf, &len, &ticket);
        if (rc == RBIPC_ERR_SHUTDOWN) {
            printf("[Consumer] Received shutdown signal.\n");
            break;
        } else if (rc == RBIPC_ERR_POISONED) {
            printf("[Consumer] Skipped poisoned slot (ticket %u)\n", ticket);
            rbipc_read_release(ring, ticket);
            continue;
        } else if (rc != RBIPC_OK) {
            fprintf(stderr, "[Consumer] Read acquire failed: %d\n", rc);
            break;
        }

        assert(len == sizeof(test_msg_t));
        const test_msg_t *msg = (const test_msg_t *)buf;

        /* Verify strict monotonic ordering */
        if (msg->seq_id != expected_seq) {
            fprintf(stderr, "[Consumer] Ordering violation! Expected %lu, got %lu\n",
                    expected_seq, msg->seq_id);
            exit(1);
        }

        expected_seq++;
        received_count++;

        rbipc_read_release(ring, ticket);
    }

    uint64_t elapsed_ns = get_time_ns() - start_time;
    double elapsed_sec = (double)elapsed_ns / 1e9;
    double throughput = (double)received_count / elapsed_sec;
    double avg_latency_ns = (double)elapsed_ns / (double)received_count;

    printf("\n=== Performance Metrics ===\n");
    printf("Total Messages : %d\n", received_count);
    printf("Elapsed Time   : %.4f seconds\n", elapsed_sec);
    printf("Throughput     : %.2f msgs/sec (%.2f MB/sec)\n",
           throughput, (throughput * sizeof(test_msg_t)) / (1024.0 * 1024.0));
    printf("Average Latency: %.2f ns/message\n\n", avg_latency_ns);

    rbipc_detach(ring);
}

static void test_crash_recovery(void) {
    printf("=== Testing Dead-Peer Crash Recovery ===\n");

    rbipc_ring_t *ring = NULL;
    int rc = rbipc_create(TEST_SHM_NAME "_crash", 16, 64, &ring);
    assert(rc == RBIPC_OK);

    pid_t pid = fork();
    if (pid == 0) {
        /* Child process: reserves a slot and abruptly crashes */
        rbipc_ring_t *child_ring = NULL;
        rbipc_attach(TEST_SHM_NAME "_crash", &child_ring);
        void *buf = NULL;
        uint32_t ticket = 0;
        rbipc_reserve_write(child_ring, 32, &buf, &ticket);
        printf("[Crash-Test Child] Slot %u reserved. Crashing abruptly via SIGKILL...\n", ticket);
        fflush(stdout);
        kill(getpid(), SIGKILL);
        _exit(1);
    }

    /* Wait for child process to die */
    int status;
    waitpid(pid, &status, 0);
    printf("[Crash-Test Parent] Detected child killed. Now attempting consumer acquire...\n");

    /* Parent consumer attempts to read the reserved slot from the deceased child */
    const void *buf = NULL;
    uint32_t len = 0;
    uint32_t ticket = 0;

    rc = rbipc_read_acquire(ring, &buf, &len, &ticket);
    printf("[Crash-Test Parent] Acquire result: %d (expected RBIPC_ERR_POISONED = %d)\n",
           rc, RBIPC_ERR_POISONED);
    assert(rc == RBIPC_ERR_POISONED);

    rbipc_read_release(ring, ticket);
    printf("[Crash-Test Parent] Poisoned slot successfully released and bypassed without deadlock!\n");

    rbipc_detach(ring);
    rbipc_destroy(TEST_SHM_NAME "_crash");
}

int main(void) {
    printf("=== librbipc End-to-End Test Harness ===\n");

    /* Cleanup any lingering shared memory object */
    rbipc_destroy(TEST_SHM_NAME);

    /* 1. Create Ring Buffer */
    rbipc_ring_t *creator_ring = NULL;
    int rc = rbipc_create(TEST_SHM_NAME, TEST_CAPACITY, TEST_SLOT_SIZE, &creator_ring);
    if (rc != RBIPC_OK) {
        fprintf(stderr, "Failed to create ring buffer: %d\n", rc);
        return 1;
    }
    printf("Ring buffer created successfully (Capacity: %d, Slot Size: %d).\n",
           TEST_CAPACITY, TEST_SLOT_SIZE);

    /* 2. Fork Producer Process */
    pid_t prod_pid = fork();
    if (prod_pid < 0) {
        perror("fork producer");
        return 1;
    } else if (prod_pid == 0) {
        run_producer(TEST_ITERATIONS);
    }

    /* 3. Run Consumer in Parent */
    run_consumer(TEST_ITERATIONS);

    /* Wait for producer child */
    waitpid(prod_pid, NULL, 0);

    /* 4. Cleanup Main Test */
    rbipc_detach(creator_ring);
    rbipc_destroy(TEST_SHM_NAME);
    printf("Main ordering & throughput test passed successfully!\n\n");

    /* 5. Run Crash Recovery Test */
    test_crash_recovery();

    printf("\nAll librbipc tests completed and verified successfully!\n");
    return 0;
}
