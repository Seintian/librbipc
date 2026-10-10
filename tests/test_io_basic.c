/**
 * @file test_io_basic.c
 * @brief Unit tests for zero-copy I/O routines, non-blocking, and timeouts
 */

#include "rbipc.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

#define TEST_IO_NAME "/rbipc_io_basic_test"
#define TEST_CAPACITY 4
#define TEST_SLOT_SZ 128

static void test_io_param_validation(void) {
    rbipc_ring_t *ring = NULL;
    int rc = rbipc_create(NULL, TEST_CAPACITY, TEST_SLOT_SZ, &ring);
    assert(rc == RBIPC_OK);

    void *out_buf = NULL;
    const void *in_buf = NULL;
    uint64_t ticket = 0;
    uint32_t len = 0;

    /* Reserve validation */
    assert(rbipc_reserve_write(NULL, 32, &out_buf, &ticket) == RBIPC_ERR_INVAL);
    assert(rbipc_reserve_write(ring, 32, NULL, &ticket) == RBIPC_ERR_INVAL);
    assert(rbipc_reserve_write(ring, 32, &out_buf, NULL) == RBIPC_ERR_INVAL);
    assert(rbipc_reserve_write(ring, TEST_SLOT_SZ + 1000, &out_buf, &ticket) == RBIPC_ERR_INVAL);

    /* Commit validation */
    assert(rbipc_commit_write(NULL, 0, 32) == RBIPC_ERR_INVAL);
    assert(rbipc_commit_write(ring, 0, TEST_SLOT_SZ + 1000) == RBIPC_ERR_INVAL);

    /* Abort validation */
    assert(rbipc_abort_write(NULL, 0) == RBIPC_ERR_INVAL);

    /* Read acquire validation */
    assert(rbipc_read_acquire(NULL, &in_buf, &len, &ticket) == RBIPC_ERR_INVAL);
    assert(rbipc_read_acquire(ring, NULL, &len, &ticket) == RBIPC_ERR_INVAL);
    assert(rbipc_read_acquire(ring, &in_buf, NULL, &ticket) == RBIPC_ERR_INVAL);
    assert(rbipc_read_acquire(ring, &in_buf, &len, NULL) == RBIPC_ERR_INVAL);

    /* Read release validation */
    assert(rbipc_read_release(NULL, 0) == RBIPC_ERR_INVAL);

    rbipc_detach(ring);
}

static void test_nonblock_and_timeout(void) {
    rbipc_ring_t *ring = NULL;
    int rc = rbipc_create(NULL, TEST_CAPACITY, TEST_SLOT_SZ, &ring);
    assert(rc == RBIPC_OK);

    const void *in_buf = NULL;
    uint32_t read_len = 0;
    uint64_t read_ticket = 0;

    /* Read on empty ring (non-blocking) */
    assert(rbipc_read_acquire_nonblock(ring, &in_buf, &read_len, &read_ticket) == RBIPC_ERR_EMPTY);

    /* Read on empty ring (timeout) */
    assert(rbipc_read_acquire_timeout(ring, 10 * RBIPC_NSEC_PER_MSEC, &in_buf, &read_len, &read_ticket) == RBIPC_ERR_TIMEOUT);

    /* Fill all slots */
    void *write_buf = NULL;
    uint64_t write_ticket = 0;

    for (uint32_t i = 0; i < TEST_CAPACITY; ++i) {
        rc = rbipc_reserve_write_nonblock(ring, 64, &write_buf, &write_ticket);
        assert(rc == RBIPC_OK);
        assert(write_ticket == i);
        snprintf((char *)write_buf, 64, "MSG_%u", i);
        rc = rbipc_commit_write(ring, write_ticket, 64);
        assert(rc == RBIPC_OK);
    }

    /* Ring is now full! Next write should fail with RBIPC_ERR_FULL or timeout */
    assert(rbipc_reserve_write_nonblock(ring, 64, &write_buf, &write_ticket) == RBIPC_ERR_FULL);
    assert(rbipc_reserve_write_timeout(ring, 64, 10 * RBIPC_NSEC_PER_MSEC, &write_buf, &write_ticket) == RBIPC_ERR_TIMEOUT);


    /* Consume all slots and verify */
    for (uint32_t i = 0; i < TEST_CAPACITY; ++i) {
        rc = rbipc_read_acquire_nonblock(ring, &in_buf, &read_len, &read_ticket);
        assert(rc == RBIPC_OK);
        assert(read_ticket == i);
        char expected[64];
        snprintf(expected, 64, "MSG_%u", i);
        assert(strcmp((const char *)in_buf, expected) == 0);
        rc = rbipc_read_release(ring, read_ticket);
        assert(rc == RBIPC_OK);
    }

    /* Now empty again */
    assert(rbipc_read_acquire_nonblock(ring, &in_buf, &read_len, &read_ticket) == RBIPC_ERR_EMPTY);

    rbipc_detach(ring);
}

static void test_circular_wrap(void) {
    rbipc_ring_t *ring = NULL;
    int rc = rbipc_create(NULL, TEST_CAPACITY, TEST_SLOT_SZ, &ring);
    assert(rc == RBIPC_OK);

    /* Loop through 5 full capacity cycles (20 messages across 4 slots) */
    for (uint32_t i = 0; i < 20; ++i) {
        void *w_buf = NULL;
        uint64_t w_ticket = 0;
        rc = rbipc_reserve_write(ring, 32, &w_buf, &w_ticket);
        assert(rc == RBIPC_OK);
        assert(w_ticket == i);

        snprintf((char *)w_buf, 32, "CYCLE_%u", i);
        rc = rbipc_commit_write(ring, w_ticket, 32);
        assert(rc == RBIPC_OK);

        const void *r_buf = NULL;
        uint32_t r_len = 0;
        uint64_t r_ticket = 0;
        rc = rbipc_read_acquire(ring, &r_buf, &r_len, &r_ticket);
        assert(rc == RBIPC_OK);
        assert(r_ticket == i);

        char expected[32];
        snprintf(expected, 32, "CYCLE_%u", i);
        assert(strcmp((const char *)r_buf, expected) == 0);

        rc = rbipc_read_release(ring, r_ticket);
        assert(rc == RBIPC_OK);
    }

    rbipc_detach(ring);
}


static void test_stream_operations(void) {
    /* 1. NULL parameter checks */
    char buf[128] = {0};
    assert(rbipc_stream_copy(NULL, buf, 64) == RBIPC_ERR_INVAL);
    assert(rbipc_stream_copy(buf, NULL, 64) == RBIPC_ERR_INVAL);

    /* 2. Direct streaming copy accuracy across diverse sizes */
    uint8_t src[4096];
    uint8_t dst[4096];
    for (int i = 0; i < 4096; ++i) {
        src[i] = (uint8_t)(i ^ 0x5A);
    }

    size_t sizes[] = {0, 1, 15, 16, 31, 32, 63, 64, 127, 255, 256, 512, 1024, 2049, 4096};
    for (size_t s = 0; s < sizeof(sizes)/sizeof(sizes[0]); ++s) {
        size_t sz = sizes[s];
        memset(dst, 0, sizeof(dst));
        assert(rbipc_stream_copy(dst, src, sz) == RBIPC_OK);
        assert(memcmp(dst, src, sz) == 0);
    }

    /* 3. High-level rbipc_write_stream through ring buffer */
    rbipc_destroy(TEST_IO_NAME);
    rbipc_ring_t *ring = NULL;
    int rc = rbipc_create(TEST_IO_NAME, 64, 4096, &ring);
    assert(rc == RBIPC_OK);

    uint64_t ticket = 0;
    assert(rbipc_write_stream(NULL, src, 1024, &ticket) == RBIPC_ERR_INVAL);
    assert(rbipc_write_stream(ring, NULL, 1024, &ticket) == RBIPC_ERR_INVAL);
    assert(rbipc_write_stream(ring, src, 1024, NULL) == RBIPC_ERR_INVAL);

    /* Transmit 1024-byte payload */
    rc = rbipc_write_stream(ring, src, 1024, &ticket);
    assert(rc == RBIPC_OK);

    const void *rbuf = NULL;
    uint32_t rlen = 0;
    uint64_t rticket = 0;
    rc = rbipc_read_acquire(ring, &rbuf, &rlen, &rticket);
    assert(rc == RBIPC_OK);
    assert(rlen == 1024);
    assert(rticket == ticket);
    assert(memcmp(rbuf, src, 1024) == 0);
    assert(rbipc_read_release(ring, rticket) == RBIPC_OK);

    /* Test non-blocking stream write */
    rc = rbipc_write_stream_nonblock(ring, src, 2048, &ticket);
    assert(rc == RBIPC_OK);
    rc = rbipc_read_acquire_nonblock(ring, &rbuf, &rlen, &rticket);
    assert(rc == RBIPC_OK);
    assert(rlen == 2048);
    assert(memcmp(rbuf, src, 2048) == 0);
    assert(rbipc_read_release(ring, rticket) == RBIPC_OK);

    rbipc_detach(ring);
    rbipc_destroy(TEST_IO_NAME);
}

int main(void) {
    printf("[test_io_basic] Running tests...\n");
    test_io_param_validation();
    test_nonblock_and_timeout();
    test_circular_wrap();
    test_stream_operations();
    printf("[test_io_basic] All tests passed!\n");
    return 0;
}
