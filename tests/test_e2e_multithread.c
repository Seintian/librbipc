/**
 * @file test_e2e_multithread.c
 * @brief Stress test with multiple concurrent producers and consumers (MPMC)
 */

#include "rbipc.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>

#define NUM_PRODUCERS 4
#define NUM_CONSUMERS 2
#define MSGS_PER_PRODUCER 25000
#define TOTAL_MSGS (NUM_PRODUCERS * MSGS_PER_PRODUCER)

typedef struct {
    uint32_t producer_id;
    uint32_t seq;
} mpmc_msg_t;

typedef struct {
    rbipc_ring_t *ring;
    uint32_t producer_id;
} producer_arg_t;

static _Atomic uint32_t g_consumed_count;

static void *producer_worker(void *arg) {
    producer_arg_t *parg = (producer_arg_t *)arg;
    rbipc_ring_t *ring = parg->ring;

    for (uint32_t i = 0; i < MSGS_PER_PRODUCER; ++i) {
        void *buf = NULL;
        uint32_t ticket = 0;

        int rc = rbipc_reserve_write(ring, sizeof(mpmc_msg_t), &buf, &ticket);
        assert(rc == RBIPC_OK);

        mpmc_msg_t *msg = (mpmc_msg_t *)buf;
        msg->producer_id = parg->producer_id;
        msg->seq = i;

        rc = rbipc_commit_write(ring, ticket, sizeof(mpmc_msg_t));
        assert(rc == RBIPC_OK);
    }
    return NULL;
}

static void *consumer_worker(void *arg) {
    rbipc_ring_t *ring = (rbipc_ring_t *)arg;

    while (atomic_load_explicit(&g_consumed_count, memory_order_relaxed) < TOTAL_MSGS) {
        const void *buf = NULL;
        uint32_t len = 0;
        uint32_t ticket = 0;

        int rc = rbipc_read_acquire_timeout(ring, 100 * RBIPC_NSEC_PER_MSEC, &buf, &len, &ticket);
        if (rc == RBIPC_ERR_TIMEOUT || rc == RBIPC_ERR_SHUTDOWN) {
            if (atomic_load_explicit(&g_consumed_count, memory_order_relaxed) >= TOTAL_MSGS) {
                break;
            }
            continue;
        }
        assert(rc == RBIPC_OK);
        assert(len == sizeof(mpmc_msg_t));

        atomic_fetch_add_explicit(&g_consumed_count, 1, memory_order_relaxed);
        rc = rbipc_read_release(ring, ticket);
        assert(rc == RBIPC_OK);
    }
    return NULL;
}

int main(void) {
    printf("[test_e2e_multithread] Running MPMC concurrency test (%d msgs across %d producers, %d consumers)...\n",
           TOTAL_MSGS, NUM_PRODUCERS, NUM_CONSUMERS);

    atomic_init(&g_consumed_count, 0);

    rbipc_ring_t *ring = NULL;
    int rc = rbipc_create(NULL, 512, sizeof(mpmc_msg_t), &ring);
    assert(rc == RBIPC_OK);

    pthread_t producers[NUM_PRODUCERS];
    producer_arg_t pargs[NUM_PRODUCERS];
    pthread_t consumers[NUM_CONSUMERS];

    for (int i = 0; i < NUM_CONSUMERS; ++i) {
        pthread_create(&consumers[i], NULL, consumer_worker, ring);
    }

    for (int i = 0; i < NUM_PRODUCERS; ++i) {
        pargs[i].ring = ring;
        pargs[i].producer_id = (uint32_t)i;
        pthread_create(&producers[i], NULL, producer_worker, &pargs[i]);
    }

    for (int i = 0; i < NUM_PRODUCERS; ++i) {
        pthread_join(producers[i], NULL);
    }

    for (int i = 0; i < NUM_CONSUMERS; ++i) {
        pthread_join(consumers[i], NULL);
    }

    assert(atomic_load(&g_consumed_count) == TOTAL_MSGS);

    rbipc_detach(ring);
    printf("[test_e2e_multithread] MPMC concurrency test passed successfully!\n");
    return 0;
}
