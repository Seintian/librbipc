/**
 * @file test_e2e_process_stress.c
 * @brief Multi-process MPMC concurrency and wait policy stress test.
 *
 * @details Spawns distinct OS processes via fork() communicating purely through
 * POSIX shared memory. Validates strict message delivery, zero data loss,
 * payload integrity, and behavior under both RBIPC_WAIT_ZERO_SPIN and
 * RBIPC_WAIT_ADAPTIVE policies.
 *
 * @author Seintian
 * @date 2026
 * @copyright Apache License 2.0
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "rbipc.h"

#include <assert.h>
#include <errno.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SHM_RING_NAME "/test_e2e_proc_stress"
#define NUM_PRODUCERS 4
#define NUM_CONSUMERS 2
#define MSGS_PER_PRODUCER 10000
#define TOTAL_MSGS (NUM_PRODUCERS * MSGS_PER_PRODUCER)
#define RING_CAPACITY 1024
#define SLOT_SIZE 64

static inline uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

typedef struct {
    uint32_t producer_id;
    uint32_t seq;
    uint64_t timestamp_ns;
    uint8_t payload[48];
} proc_msg_t;

static_assert(sizeof(proc_msg_t) == SLOT_SIZE, "proc_msg_t must match SLOT_SIZE");

typedef struct {
    _Atomic uint32_t start_flag;
    _Atomic uint32_t total_consumed;
    _Atomic uint32_t error_count;
    _Atomic uint8_t received[NUM_PRODUCERS][MSGS_PER_PRODUCER];
} proc_coordination_t;

static void run_producer_process(uint32_t producer_id, proc_coordination_t *coord) {
    rbipc_ring_t *ring = NULL;
    int rc = rbipc_attach(SHM_RING_NAME, &ring);
    if (rc != RBIPC_OK) {
        atomic_fetch_add(&coord->error_count, 1);
        exit(1);
    }

    /* Wait for start signal */
    while (atomic_load_explicit(&coord->start_flag, memory_order_acquire) == 0) {
        sched_yield();
    }

    proc_msg_t msg;
    msg.producer_id = producer_id;

    for (uint32_t i = 0; i < MSGS_PER_PRODUCER; ++i) {
        msg.seq = i;
        msg.timestamp_ns = get_time_ns();
        for (size_t b = 0; b < sizeof(msg.payload); ++b) {
            msg.payload[b] = (uint8_t)((producer_id * 37U + i * 17U + b) & 0xFFU);
        }

        void *wbuf = NULL;
        uint64_t ticket = 0;
        rc = rbipc_reserve_write(ring, sizeof(proc_msg_t), &wbuf, &ticket);
        if (rc != RBIPC_OK) {
            atomic_fetch_add(&coord->error_count, 1);
            break;
        }

        memcpy(wbuf, &msg, sizeof(proc_msg_t));
        rc = rbipc_commit_write(ring, ticket, sizeof(proc_msg_t));
        if (rc != RBIPC_OK) {
            atomic_fetch_add(&coord->error_count, 1);
            break;
        }
    }

    rbipc_detach(ring);
    exit(0);
}

static void run_consumer_process(proc_coordination_t *coord) {
    rbipc_ring_t *ring = NULL;
    int rc = rbipc_attach(SHM_RING_NAME, &ring);
    if (rc != RBIPC_OK) {
        atomic_fetch_add(&coord->error_count, 1);
        exit(1);
    }

    /* Wait for start signal */
    while (atomic_load_explicit(&coord->start_flag, memory_order_acquire) == 0) {
        sched_yield();
    }

    while (atomic_load_explicit(&coord->total_consumed, memory_order_relaxed) < TOTAL_MSGS) {
        const void *rbuf = NULL;
        uint32_t rlen = 0;
        uint64_t ticket = 0;

        rc = rbipc_read_acquire_timeout(ring, 50 * RBIPC_NSEC_PER_MSEC, &rbuf, &rlen, &ticket);
        if (rc == RBIPC_ERR_TIMEOUT || rc == RBIPC_ERR_SHUTDOWN) {
            if (atomic_load_explicit(&coord->total_consumed, memory_order_relaxed) >= TOTAL_MSGS) {
                break;
            }
            continue;
        }
        if (rc != RBIPC_OK || rlen != sizeof(proc_msg_t)) {
            atomic_fetch_add(&coord->error_count, 1);
            break;
        }

        const proc_msg_t *msg = (const proc_msg_t *)rbuf;
        if (msg->producer_id >= NUM_PRODUCERS || msg->seq >= MSGS_PER_PRODUCER) {
            atomic_fetch_add(&coord->error_count, 1);
        } else {
            /* Verify payload bytes */
            bool payload_ok = true;
            for (size_t b = 0; b < sizeof(msg->payload); ++b) {
                uint8_t expected = (uint8_t)((msg->producer_id * 37U + msg->seq * 17U + b) & 0xFFU);
                if (msg->payload[b] != expected) {
                    payload_ok = false;
                    break;
                }
            }
            if (!payload_ok) {
                atomic_fetch_add(&coord->error_count, 1);
            }

            /* Record reception */
            uint8_t prev = atomic_exchange_explicit(
                &coord->received[msg->producer_id][msg->seq], 1, memory_order_relaxed);
            if (prev != 0) {
                /* Duplicate message received */
                atomic_fetch_add(&coord->error_count, 1);
            }
        }

        atomic_fetch_add_explicit(&coord->total_consumed, 1, memory_order_relaxed);
        rc = rbipc_read_release(ring, ticket);
        if (rc != RBIPC_OK) {
            atomic_fetch_add(&coord->error_count, 1);
            break;
        }
    }

    rbipc_detach(ring);
    exit(0);
}

static void test_process_stress_round(uint32_t wait_policy, const char *policy_desc) {
    printf("  [test_e2e_process_stress] Running round with %s...\n", policy_desc);

    rbipc_destroy(SHM_RING_NAME);

    /* Allocate shared coordination region */
    proc_coordination_t *coord = (proc_coordination_t *)mmap(
        NULL, sizeof(proc_coordination_t), PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    assert(coord != MAP_FAILED);
    memset(coord, 0, sizeof(proc_coordination_t));

    /* Create shared memory ring */
    rbipc_ring_t *master_ring = NULL;
    int rc = rbipc_create(SHM_RING_NAME, RING_CAPACITY, SLOT_SIZE, &master_ring);
    assert(rc == RBIPC_OK);
    assert(rbipc_set_wait_policy(master_ring, wait_policy) == RBIPC_OK);

    pid_t consumer_pids[NUM_CONSUMERS];
    pid_t producer_pids[NUM_PRODUCERS];

    /* Fork consumer processes */
    for (int i = 0; i < NUM_CONSUMERS; ++i) {
        pid_t pid = fork();
        assert(pid >= 0);
        if (pid == 0) {
            run_consumer_process(coord);
        }
        consumer_pids[i] = pid;
    }

    /* Fork producer processes */
    for (int i = 0; i < NUM_PRODUCERS; ++i) {
        pid_t pid = fork();
        assert(pid >= 0);
        if (pid == 0) {
            run_producer_process((uint32_t)i, coord);
        }
        producer_pids[i] = pid;
    }

    /* Allow children to initialize and attach, then give start signal */
    usleep(10000); /* 10ms */
    atomic_store_explicit(&coord->start_flag, 1, memory_order_release);

    /* Wait for producers to complete */
    for (int i = 0; i < NUM_PRODUCERS; ++i) {
        int status = 0;
        pid_t p = waitpid(producer_pids[i], &status, 0);
        assert(p == producer_pids[i]);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }

    /* Wait for consumers to complete */
    for (int i = 0; i < NUM_CONSUMERS; ++i) {
        int status = 0;
        pid_t p = waitpid(consumer_pids[i], &status, 0);
        assert(p == consumer_pids[i]);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }

    /* Verify coordination metrics */
    assert(atomic_load(&coord->error_count) == 0);
    assert(atomic_load(&coord->total_consumed) == TOTAL_MSGS);

    /* Verify every single message was received exactly once */
    for (uint32_t p = 0; p < NUM_PRODUCERS; ++p) {
        for (uint32_t s = 0; s < MSGS_PER_PRODUCER; ++s) {
            assert(atomic_load(&coord->received[p][s]) == 1);
        }
    }

    rbipc_detach(master_ring);
    rbipc_destroy(SHM_RING_NAME);
    munmap(coord, sizeof(proc_coordination_t));
}

int main(void) {
    printf("[test_e2e_process_stress] Running multi-process MPMC stress test (%d msgs across %d producers, %d consumers)...\n",
           TOTAL_MSGS, NUM_PRODUCERS, NUM_CONSUMERS);

    test_process_stress_round(RBIPC_WAIT_ZERO_SPIN, "RBIPC_WAIT_ZERO_SPIN");
    test_process_stress_round(RBIPC_WAIT_ADAPTIVE, "RBIPC_WAIT_ADAPTIVE");

    printf("[test_e2e_process_stress] All multi-process stress tests passed successfully.\n");
    return 0;
}
