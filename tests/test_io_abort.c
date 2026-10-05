/**
 * @file test_io_abort.c
 * @brief Test write abort behavior and recovery
 */

#include "rbipc.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

int main(void) {
    printf("[test_io_abort] Running tests...\n");

    rbipc_ring_t *ring = NULL;
    int rc = rbipc_create(NULL, 8, 128, &ring);
    assert(rc == RBIPC_OK);

    /* 1. Normal write */
    void *w_buf = NULL;
    uint32_t ticket0 = 0;
    rc = rbipc_reserve_write(ring, 32, &w_buf, &ticket0);
    assert(rc == RBIPC_OK);
    strcpy((char *)w_buf, "MSG_0");
    rc = rbipc_commit_write(ring, ticket0, 32);
    assert(rc == RBIPC_OK);

    /* 2. Aborted write */
    uint32_t ticket1 = 0;
    rc = rbipc_reserve_write(ring, 32, &w_buf, &ticket1);
    assert(rc == RBIPC_OK);
    /* Decide to abort */
    rc = rbipc_abort_write(ring, ticket1);
    assert(rc == RBIPC_OK);

    /* 3. Normal write again */
    uint32_t ticket2 = 0;
    rc = rbipc_reserve_write(ring, 32, &w_buf, &ticket2);
    assert(rc == RBIPC_OK);
    strcpy((char *)w_buf, "MSG_2");
    rc = rbipc_commit_write(ring, ticket2, 32);
    assert(rc == RBIPC_OK);

    /* Consumer side */
    const void *r_buf = NULL;
    uint32_t r_len = 0;
    uint32_t r_ticket = 0;

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

    rbipc_detach(ring);
    printf("[test_io_abort] All tests passed!\n");
    return 0;
}
