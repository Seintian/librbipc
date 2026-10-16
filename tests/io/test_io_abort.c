/**
 * @file test_io_abort.c
 * @brief Test write abort behavior and recovery
 */

#include "rbipc.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <unistd.h>

typedef struct {
    rbipc_ring_t *ring;
    int rc;
    uint64_t ticket;
} abort_waiter_t;

static void *abort_waiter_worker(void *arg) {
    abort_waiter_t *w = (abort_waiter_t *)arg;
    const void *buf = NULL;
    uint32_t len = 0;
    w->rc = rbipc_read_acquire_timeout(w->ring, 2000000000ULL, &buf, &len, &w->ticket);
    return NULL;
}

int main(void) {
    printf("[test_io_abort] Running tests...\n");

    rbipc_ring_t *ring = NULL;
    int rc = rbipc_create(NULL, 8, 128, &ring);
    assert(rc == RBIPC_OK);

    /* Negative argument check */
    assert(rbipc_abort_write(NULL, 0) == RBIPC_ERR_INVAL);

    /* 1. Normal write */
    void *w_buf = NULL;
    uint64_t ticket0 = 0;
    rc = rbipc_reserve_write(ring, 32, &w_buf, &ticket0);
    assert(rc == RBIPC_OK);
    strcpy((char *)w_buf, "MSG_0");
    rc = rbipc_commit_write(ring, ticket0, 32);
    assert(rc == RBIPC_OK);

    /* Aborting already committed ticket must fail with RBIPC_ERR_BUSY */
    assert(rbipc_abort_write(ring, ticket0) == RBIPC_ERR_BUSY);

    /* 2. Aborted write */
    uint64_t ticket1 = 0;
    rc = rbipc_reserve_write(ring, 32, &w_buf, &ticket1);
    assert(rc == RBIPC_OK);
    /* Decide to abort */
    rc = rbipc_abort_write(ring, ticket1);
    assert(rc == RBIPC_OK);

    /* 3. Normal write again */
    uint64_t ticket2 = 0;
    rc = rbipc_reserve_write(ring, 32, &w_buf, &ticket2);
    assert(rc == RBIPC_OK);
    strcpy((char *)w_buf, "MSG_2");
    rc = rbipc_commit_write(ring, ticket2, 32);
    assert(rc == RBIPC_OK);

    /* Consumer side */
    const void *r_buf = NULL;
    uint32_t r_len = 0;
    uint64_t r_ticket = 0;

    /* Read MSG_0 */
    rc = rbipc_read_acquire(ring, &r_buf, &r_len, &r_ticket);
    assert(rc == RBIPC_OK);
    assert(r_ticket == ticket0);
    assert(strcmp((const char *)r_buf, "MSG_0") == 0);
    rbipc_read_release(ring, r_ticket);

    /* Read ticket 1: should return RBIPC_ERR_POISONED */
    rc = rbipc_read_acquire(ring, &r_buf, &r_len, &r_ticket);
    assert(rc == RBIPC_ERR_POISONED);
    assert(r_ticket == ticket1);
    rbipc_read_release(ring, r_ticket);

    /* Read MSG_2 */
    rc = rbipc_read_acquire(ring, &r_buf, &r_len, &r_ticket);
    assert(rc == RBIPC_OK);
    assert(r_ticket == ticket2);
    assert(strcmp((const char *)r_buf, "MSG_2") == 0);
    rbipc_read_release(ring, r_ticket);

    /* 4. Concurrent reader awakened by abort */
    abort_waiter_t waiter = {
        .ring = ring,
        .rc = -999,
        .ticket = 0
    };
    pthread_t th;
    assert(pthread_create(&th, NULL, abort_waiter_worker, &waiter) == 0);

    /* Allow reader to enter futex wait on slot */
    usleep(20000); /* 20ms */

    uint64_t ticket3 = 0;
    rc = rbipc_reserve_write(ring, 32, &w_buf, &ticket3);
    assert(rc == RBIPC_OK);

    /* Abort while reader is waiting */
    assert(rbipc_abort_write(ring, ticket3) == RBIPC_OK);

    pthread_join(th, NULL);
    assert(waiter.rc == RBIPC_ERR_POISONED);
    assert(waiter.ticket == ticket3);
    rbipc_read_release(ring, waiter.ticket);

    rbipc_detach(ring);
    printf("[test_io_abort] All tests passed!\n");
    return 0;
}
