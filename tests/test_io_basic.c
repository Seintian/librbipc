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
    char buf[128] = {};
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

static void test_convenience_copy_io(void) {
    rbipc_ring_t *ring = NULL;
    int rc = rbipc_create(NULL, TEST_CAPACITY, TEST_SLOT_SZ, &ring);
    assert(rc == RBIPC_OK);

    char buf[TEST_SLOT_SZ];
    uint32_t out_len = 0;

    /* 1. Parameter validation */
    assert(rbipc_write(NULL, "data", 4) == RBIPC_ERR_INVAL);
    assert(rbipc_write(ring, NULL, 4) == RBIPC_ERR_INVAL);
    assert(rbipc_write_timeout(NULL, "data", 4, 1000) == RBIPC_ERR_INVAL);
    assert(rbipc_write_timeout(ring, NULL, 4, 1000) == RBIPC_ERR_INVAL);
    assert(rbipc_write_nonblock(NULL, "data", 4) == RBIPC_ERR_INVAL);
    assert(rbipc_write_nonblock(ring, NULL, 4) == RBIPC_ERR_INVAL);

    assert(rbipc_read(NULL, buf, sizeof(buf), &out_len) == RBIPC_ERR_INVAL);
    assert(rbipc_read(ring, NULL, sizeof(buf), &out_len) == RBIPC_ERR_INVAL);
    assert(rbipc_read_timeout(NULL, buf, sizeof(buf), &out_len, 1000) == RBIPC_ERR_INVAL);
    assert(rbipc_read_timeout(ring, NULL, sizeof(buf), &out_len, 1000) == RBIPC_ERR_INVAL);
    assert(rbipc_read_nonblock(NULL, buf, sizeof(buf), &out_len) == RBIPC_ERR_INVAL);
    assert(rbipc_read_nonblock(ring, NULL, sizeof(buf), &out_len) == RBIPC_ERR_INVAL);

    /* 2. Empty ring read checks */
    assert(rbipc_read_nonblock(ring, buf, sizeof(buf), &out_len) == RBIPC_ERR_EMPTY);
    assert(rbipc_read_timeout(ring, buf, sizeof(buf), &out_len, 1000000ULL) == RBIPC_ERR_TIMEOUT);

    /* 3. Successful write and read */
    const char *test_msg = "Hello rbipc convenience!";
    uint32_t msg_len = (uint32_t)strlen(test_msg) + 1;
    rc = rbipc_write(ring, test_msg, msg_len);
    assert(rc == RBIPC_OK);

    memset(buf, 0, sizeof(buf));
    rc = rbipc_read(ring, buf, sizeof(buf), &out_len);
    assert(rc == RBIPC_OK);
    assert(out_len == msg_len);
    assert(strcmp(buf, test_msg) == 0);

    /* 4. Truncated read and NULL out_len */
    rc = rbipc_write_nonblock(ring, "1234567890", 10);
    assert(rc == RBIPC_OK);
    char small_buf[5] = {};
    rc = rbipc_read(ring, small_buf, 4, &out_len);
    assert(rc == RBIPC_OK);
    assert(out_len == 10);
    assert(memcmp(small_buf, "1234", 4) == 0);

    /* Read with NULL out_len */
    rc = rbipc_write_nonblock(ring, "xyz", 3);
    assert(rc == RBIPC_OK);
    rc = rbipc_read_nonblock(ring, buf, sizeof(buf), NULL);
    assert(rc == RBIPC_OK);
    assert(memcmp(buf, "xyz", 3) == 0);

    /* 5. Fill ring buffer to test full and timeout */
    for (uint32_t i = 0; i < TEST_CAPACITY; ++i) {
        char item[16];
        snprintf(item, sizeof(item), "ITEM_%u", i);
        rc = rbipc_write_nonblock(ring, item, (uint32_t)strlen(item) + 1);
        assert(rc == RBIPC_OK);
    }
    /* Ring full */
    assert(rbipc_write_nonblock(ring, "overflow", 8) == RBIPC_ERR_FULL);
    assert(rbipc_write_timeout(ring, "overflow", 8, 1000000ULL) == RBIPC_ERR_TIMEOUT);

    /* Drain all */
    for (uint32_t i = 0; i < TEST_CAPACITY; ++i) {
        char item[16];
        rc = rbipc_read_nonblock(ring, item, sizeof(item), NULL);
        assert(rc == RBIPC_OK);
        char expected[16];
        snprintf(expected, sizeof(expected), "ITEM_%u", i);
        assert(strcmp(item, expected) == 0);
    }
    assert(rbipc_read_nonblock(ring, buf, sizeof(buf), NULL) == RBIPC_ERR_EMPTY);

    /* 6. Test rbipc_shutdown alias */
    assert(rbipc_shutdown(NULL) == RBIPC_ERR_INVAL);
    assert(rbipc_shutdown(ring) == RBIPC_OK);
    assert(rbipc_write(ring, "test", 4) == RBIPC_ERR_SHUTDOWN);
    assert(rbipc_read(ring, buf, sizeof(buf), NULL) == RBIPC_ERR_SHUTDOWN);

    rbipc_detach(ring);
}

int main(void) {
    printf("[test_io_basic] Running tests...\n");
    test_io_param_validation();
    test_nonblock_and_timeout();
    test_circular_wrap();
    test_stream_operations();
    test_convenience_copy_io();
    printf("[test_io_basic] All tests passed!\n");
    return 0;
}
