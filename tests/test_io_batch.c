/**
 * @file test_io_batch.c
 * @brief Unit and integration tests for B-Queue batching operations
 */

#include "rbipc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define BATCH_RING_NAME "/rbipc_test_batch"
#define BATCH_CAPACITY  64
#define BATCH_SLOT_SIZE 128

static void test_batch_validation(void) {
    rbipc_ring_t *ring = NULL;
    int rc = rbipc_create(BATCH_RING_NAME, BATCH_CAPACITY, BATCH_SLOT_SIZE, &ring);
    assert(rc == RBIPC_OK);

    rbipc_iovec_t iovecs[16];
    uint32_t reserved = 0;
    rbipc_rovec_t rovecs[16];
    uint32_t acquired = 0;
    uint32_t tickets[16] = {0};
    uint32_t lens[16] = {0};

    /* NULL / invalid argument tests */
    assert(rbipc_reserve_write_batch(NULL, 4, iovecs, &reserved) == RBIPC_ERR_INVAL);
    assert(rbipc_reserve_write_batch(ring, 0, iovecs, &reserved) == RBIPC_ERR_INVAL);
    assert(rbipc_reserve_write_batch(ring, 4, NULL, &reserved) == RBIPC_ERR_INVAL);
    assert(rbipc_reserve_write_batch(ring, 4, iovecs, NULL) == RBIPC_ERR_INVAL);

    assert(rbipc_commit_write_batch(NULL, 4, tickets, lens) == RBIPC_ERR_INVAL);
    assert(rbipc_commit_write_batch(ring, 0, tickets, lens) == RBIPC_ERR_INVAL);
    assert(rbipc_commit_write_batch(ring, 4, NULL, lens) == RBIPC_ERR_INVAL);
    assert(rbipc_commit_write_batch(ring, 4, tickets, NULL) == RBIPC_ERR_INVAL);

    assert(rbipc_read_acquire_batch(NULL, 4, rovecs, &acquired) == RBIPC_ERR_INVAL);
    assert(rbipc_read_acquire_batch(ring, 0, rovecs, &acquired) == RBIPC_ERR_INVAL);
    assert(rbipc_read_acquire_batch(ring, 4, NULL, &acquired) == RBIPC_ERR_INVAL);
    assert(rbipc_read_acquire_batch(ring, 4, rovecs, NULL) == RBIPC_ERR_INVAL);

    assert(rbipc_read_release_batch(NULL, 4, tickets) == RBIPC_ERR_INVAL);
    assert(rbipc_read_release_batch(ring, 0, tickets) == RBIPC_ERR_INVAL);
    assert(rbipc_read_release_batch(ring, 4, NULL) == RBIPC_ERR_INVAL);

    /* Commit with invalid length exceeding slot size */
    lens[0] = BATCH_SLOT_SIZE + 100;
    assert(rbipc_commit_write_batch(ring, 1, tickets, lens) == RBIPC_ERR_INVAL);

    rbipc_detach(ring);
    rbipc_destroy(BATCH_RING_NAME);
}

static void test_batch_roundtrip(void) {
    rbipc_ring_t *ring = NULL;
    int rc = rbipc_create(BATCH_RING_NAME, BATCH_CAPACITY, BATCH_SLOT_SIZE, &ring);
    assert(rc == RBIPC_OK);

    const uint32_t batch_size = 16;
    rbipc_iovec_t iovecs[16];
    uint32_t reserved = 0;

    /* Reserve a batch of 16 slots */
    rc = rbipc_reserve_write_batch(ring, batch_size, iovecs, &reserved);
    assert(rc == RBIPC_OK);
    assert(reserved == batch_size);

    uint32_t tickets[16];
    uint32_t lens[16];

    for (uint32_t i = 0; i < batch_size; ++i) {
        assert(iovecs[i].buf != NULL);
        assert(iovecs[i].ticket == i);
        assert(iovecs[i].max_len >= BATCH_SLOT_SIZE);

        char msg[64];
        snprintf(msg, sizeof(msg), "BATCH_MESSAGE_%u", i);
        memcpy(iovecs[i].buf, msg, strlen(msg) + 1);

        tickets[i] = iovecs[i].ticket;
        lens[i] = (uint32_t)strlen(msg) + 1;
    }

    /* Commit the batch */
    rc = rbipc_commit_write_batch(ring, batch_size, tickets, lens);
    assert(rc == RBIPC_OK);

    /* Acquire the batch in 2 chunks of 8 */
    rbipc_rovec_t rovecs[8];
    uint32_t acquired = 0;

    rc = rbipc_read_acquire_batch(ring, 8, rovecs, &acquired);
    assert(rc == RBIPC_OK);
    assert(acquired == 8);

    for (uint32_t i = 0; i < 8; ++i) {
        char expected[64];
        snprintf(expected, sizeof(expected), "BATCH_MESSAGE_%u", i);
        assert(rovecs[i].buf != NULL);
        assert(rovecs[i].len == strlen(expected) + 1);
        assert(strcmp((const char *)rovecs[i].buf, expected) == 0);
        assert(rovecs[i].ticket == i);
        tickets[i] = rovecs[i].ticket;
    }

    /* Release first 8 */
    rc = rbipc_read_release_batch(ring, 8, tickets);
    assert(rc == RBIPC_OK);

    /* Acquire next 8 */
    rc = rbipc_read_acquire_batch(ring, 8, rovecs, &acquired);
    assert(rc == RBIPC_OK);
    assert(acquired == 8);

    for (uint32_t i = 0; i < 8; ++i) {
        char expected[64];
        snprintf(expected, sizeof(expected), "BATCH_MESSAGE_%u", i + 8);
        assert(rovecs[i].buf != NULL);
        assert(rovecs[i].len == strlen(expected) + 1);
        assert(strcmp((const char *)rovecs[i].buf, expected) == 0);
        assert(rovecs[i].ticket == i + 8);
        tickets[i] = rovecs[i].ticket;
    }

    /* Release next 8 */
    rc = rbipc_read_release_batch(ring, 8, tickets);
    assert(rc == RBIPC_OK);

    /* Test poison handling in batch read */
    rc = rbipc_reserve_write_batch(ring, 2, iovecs, &reserved);
    assert(rc == RBIPC_OK);
    assert(reserved == 2);

    /* Commit first slot, abort second slot */
    memcpy(iovecs[0].buf, "COMMITTED_SLOT", 15);
    uint32_t t_commit = iovecs[0].ticket;
    uint32_t l_commit = 15;
    assert(rbipc_commit_write_batch(ring, 1, &t_commit, &l_commit) == RBIPC_OK);
    assert(rbipc_abort_write(ring, iovecs[1].ticket) == RBIPC_OK);

    rc = rbipc_read_acquire_batch(ring, 2, rovecs, &acquired);
    assert(rc == RBIPC_OK);
    assert(acquired == 2);
    assert(rovecs[0].buf != NULL);
    assert(rovecs[0].len == 15);
    assert(rovecs[1].buf == NULL); /* Poisoned slot */
    assert(rovecs[1].len == 0);

    uint32_t rel_tickets[2] = {rovecs[0].ticket, rovecs[1].ticket};
    assert(rbipc_read_release_batch(ring, 2, rel_tickets) == RBIPC_OK);

    rbipc_detach(ring);
    rbipc_destroy(BATCH_RING_NAME);
}

int main(void) {
    printf("[test_io_batch] Running B-Queue batching tests...\n");
    test_batch_validation();
    test_batch_roundtrip();
    printf("[test_io_batch] All tests passed!\n");
    return 0;
}
