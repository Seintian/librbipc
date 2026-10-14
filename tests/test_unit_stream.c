/**
 * @file test_unit_stream.c
 * @brief Unit tests for non-temporal streaming store memory engine.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "rbipc.h"
#include "../src/rbipc_stream.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_stream_ring_integration(void);

int main(void) {
    printf("[test_unit_stream] Running streaming copy tests...\n");

    /* Defensive tests */
    assert(rbipc_stream_copy(NULL, NULL, 100) == RBIPC_ERR_INVAL);

    /* Test sizes: 1 B, 63 B, 64 B, 255 B, 256 B, 512 B, 4096 B, 65536 B */
    const size_t test_sizes[] = { 1, 15, 16, 31, 32, 63, 64, 127, 255, 256, 512, 1024, 4096, 65536 };
    const size_t num_tests = sizeof(test_sizes) / sizeof(test_sizes[0]);

    for (size_t t = 0; t < num_tests; ++t) {
        size_t sz = test_sizes[t];
        uint8_t *src = (uint8_t *)malloc(sz + 64);
        uint8_t *dst = (uint8_t *)malloc(sz + 64);
        assert(src && dst);

        for (size_t i = 0; i < sz; ++i) {
            src[i] = (uint8_t)((i * 37 + 11) & 0xFF);
        }
        memset(dst, 0, sz);

        /* Test aligned copy */
        int rc = rbipc_stream_copy(dst, src, sz);
        assert(rc == RBIPC_OK);
        assert(memcmp(src, dst, sz) == 0);

        /* Test unaligned offset destination */
        for (size_t off = 1; off < 16; ++off) {
            memset(dst + off, 0, sz);
            rc = rbipc_stream_copy(dst + off, src, sz);
            assert(rc == RBIPC_OK);
            assert(memcmp(src, dst + off, sz) == 0);
        }

        /* Test rbipc_stream_copy_auto */
        memset(dst, 0, sz);
        rbipc_stream_copy_auto(dst, src, sz);
        assert(memcmp(src, dst, sz) == 0);

        free(src);
        free(dst);
    }

    test_stream_ring_integration();

    printf("[test_unit_stream] All tests passed!\n");
    return 0;
}

static void test_stream_ring_integration(void) {
    rbipc_ring_t *ring = NULL;
    assert(rbipc_create(NULL, 4, 256, &ring) == RBIPC_OK);

    uint64_t ticket = 0;
    char payload[256];
    memset(payload, 0x42, sizeof(payload));

    /* Negative parameter validation */
    assert(rbipc_write_stream_timeout(NULL, payload, 32, 0, &ticket) == RBIPC_ERR_INVAL);
    assert(rbipc_write_stream_timeout(ring, NULL, 32, 0, &ticket) == RBIPC_ERR_INVAL);
    assert(rbipc_write_stream_timeout(ring, payload, 32, 0, NULL) == RBIPC_ERR_INVAL);
    assert(rbipc_write_stream_nonblock(NULL, payload, 32, &ticket) == RBIPC_ERR_INVAL);
    assert(rbipc_write_stream(NULL, payload, 32, &ticket) == RBIPC_ERR_INVAL);
    assert(rbipc_write_stream(ring, payload, 300, &ticket) == RBIPC_ERR_INVAL);

    /* Fill ring using stream writes */
    for (int i = 0; i < 4; ++i) {
        payload[0] = (char)i;
        assert(rbipc_write_stream(ring, payload, sizeof(payload), &ticket) == RBIPC_OK);
        assert(ticket == (uint64_t)i);
    }

    /* Ring is now full */
    assert(rbipc_write_stream_nonblock(ring, payload, 64, &ticket) == RBIPC_ERR_FULL);
    assert(rbipc_write_stream_timeout(ring, payload, 64, 5000000ULL /* 5ms */, &ticket) == RBIPC_ERR_TIMEOUT);

    /* Consume slots and verify payload */
    for (int i = 0; i < 4; ++i) {
        const void *rbuf = NULL;
        uint32_t rlen = 0;
        uint64_t rticket = 0;
        assert(rbipc_read_acquire(ring, &rbuf, &rlen, &rticket) == RBIPC_OK);
        assert(rlen == sizeof(payload));
        assert(rticket == (uint64_t)i);
        const uint8_t *bytes = (const uint8_t *)rbuf;
        assert(bytes[0] == (uint8_t)i);
        for (size_t b = 1; b < sizeof(payload); ++b) {
            assert(bytes[b] == 0x42);
        }
        assert(rbipc_read_release(ring, rticket) == RBIPC_OK);
    }

    /* Now write again with nonblock, should succeed */
    assert(rbipc_write_stream_nonblock(ring, payload, 64, &ticket) == RBIPC_OK);
    assert(ticket == 4);

    rbipc_detach(ring);
}