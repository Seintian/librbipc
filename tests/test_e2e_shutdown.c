/**
 * @file test_e2e_shutdown.c
 * @brief Test shutdown signaling waking blocked producers and consumers
 */

#include "rbipc.h"
#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#include <pthread.h>

static void *consumer_thread(void *arg) {
    rbipc_ring_t *ring = (rbipc_ring_t *)arg;
    const void *buf = NULL;
    uint32_t len = 0;
    uint32_t ticket = 0;

    int rc = rbipc_read_acquire(ring, &buf, &len, &ticket);
    assert(rc == RBIPC_ERR_SHUTDOWN);
    return NULL;
}

static void *producer_thread(void *arg) {
    rbipc_ring_t *ring = (rbipc_ring_t *)arg;
    void *buf = NULL;
    uint32_t ticket = 0;

    /* Fill buffer first */
    for (int i = 0; i < 4; ++i) {
        int rc = rbipc_reserve_write(ring, 32, &buf, &ticket);
        assert(rc == RBIPC_OK);
        rbipc_commit_write(ring, ticket, 32);
    }

    /* Next write will block because buffer is full */
    int rc = rbipc_reserve_write(ring, 32, &buf, &ticket);
    assert(rc == RBIPC_ERR_SHUTDOWN);
    return NULL;
}

int main(void) {
    printf("[test_e2e_shutdown] Running tests...\n");

    /* 1. Test consumer blocking on empty */
    rbipc_ring_t *ring1 = NULL;
    int rc = rbipc_create(NULL, 4, 64, &ring1);
    assert(rc == RBIPC_OK);

    pthread_t c_th;
    pthread_create(&c_th, NULL, consumer_thread, ring1);

    usleep(50000); /* 50ms to ensure consumer is sleeping on futex */
    rbipc_signal_shutdown(ring1);
    pthread_join(c_th, NULL);
    rbipc_detach(ring1);

    /* 2. Test producer blocking on full */
    rbipc_ring_t *ring2 = NULL;
    rc = rbipc_create(NULL, 4, 64, &ring2);
    assert(rc == RBIPC_OK);

    pthread_t p_th;
    pthread_create(&p_th, NULL, producer_thread, ring2);

    usleep(50000); /* 50ms to ensure producer filled ring and is sleeping on futex */
    rbipc_signal_shutdown(ring2);
    pthread_join(p_th, NULL);
    rbipc_detach(ring2);

    printf("[test_e2e_shutdown] All tests passed!\n");
    return 0;
}
