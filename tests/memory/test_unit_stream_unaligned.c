/**
 * @file test_unit_stream_unaligned.c
 * @brief Unit tests for unaligned streaming copy combinations, multi-block
 *        streaming store loops, and defensive boundary validations.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "rbipc.h"
#include "rbipc_stream.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_streaming_exhaustive_sizes(void) {
    const size_t sizes[] = {
        0, 1, 2, 3, 7, 15, 16, 23, 31, 32, 47, 63, 64, 79, 95,
        127, 128, 191, 255, 256, 257, 287, 319, 351, 383, 415,
        511, 512, 1024, 2048, 4096
    };
    const size_t num_sizes = sizeof(sizes) / sizeof(sizes[0]);
    const size_t max_align_pad = 128;

    for (size_t s = 0; s < num_sizes; ++s) {
        size_t sz = sizes[s];
        uint8_t *src_buf = (uint8_t *)malloc(sz + max_align_pad);
        uint8_t *dst_buf = (uint8_t *)malloc(sz + max_align_pad);
        assert(src_buf && dst_buf);

        /* Initialize pseudo-random data */
        for (size_t i = 0; i < sz + max_align_pad; ++i) {
            src_buf[i] = (uint8_t)((i * 31 + 17) & 0xFF);
        }

        /* Test various alignment offsets for dst and src */
        const size_t offsets[] = {0, 1, 3, 7, 15, 16, 31, 32, 47, 63, 64};
        const size_t num_offsets = sizeof(offsets) / sizeof(offsets[0]);

        for (size_t doff_idx = 0; doff_idx < num_offsets; ++doff_idx) {
            size_t dst_off = offsets[doff_idx];
            for (size_t soff_idx = 0; soff_idx < num_offsets; ++soff_idx) {
                size_t src_off = offsets[soff_idx];

                memset(dst_buf, 0xCC, sz + max_align_pad);
                int rc = rbipc_stream_copy(dst_buf + dst_off, src_buf + src_off, sz);
                assert(rc == RBIPC_OK);

                if (sz > 0) {
                    assert(memcmp(dst_buf + dst_off, src_buf + src_off, sz) == 0);
                }
            }
        }

        free(src_buf);
        free(dst_buf);
    }
}

static void test_streaming_defensive_nulls(void) {
    uint8_t dummy_dst[64] = {0};
    uint8_t dummy_src[64] = {0};
    assert(rbipc_stream_copy(NULL, dummy_src, 64) == RBIPC_ERR_INVAL);
    assert(rbipc_stream_copy(dummy_dst, NULL, 64) == RBIPC_ERR_INVAL);
    assert(rbipc_stream_copy(NULL, NULL, 64) == RBIPC_ERR_INVAL);
    assert(rbipc_stream_copy(dummy_dst, dummy_src, 0) == RBIPC_OK);
}

static void test_write_stream_lifecycle(void) {
    rbipc_ring_t *ring = NULL;
    assert(rbipc_create(NULL, 8, 512, &ring) == RBIPC_OK);

    uint8_t payload[512];
    for (size_t i = 0; i < sizeof(payload); ++i) {
        payload[i] = (uint8_t)(i & 0xFF);
    }

    uint64_t ticket = 0;
    /* Stream write with excessive length must fail */
    assert(rbipc_write_stream(ring, payload, 513, &ticket) == RBIPC_ERR_INVAL);

    /* Stream write with zero length is valid empty message */
    assert(rbipc_write_stream(ring, payload, 0, &ticket) == RBIPC_OK);
    assert(ticket == 0);

    /* Consume zero-length message */
    const void *empty_data = NULL;
    uint32_t empty_len = 999;
    uint64_t empty_ticket = 999;
    assert(rbipc_read_acquire(ring, &empty_data, &empty_len, &empty_ticket) == RBIPC_OK);
    assert(empty_len == 0);
    assert(empty_ticket == 0);
    assert(rbipc_read_release(ring, empty_ticket) == RBIPC_OK);

    /* Stream write valid full payload */
    assert(rbipc_write_stream(ring, payload, 512, &ticket) == RBIPC_OK);
    assert(ticket == 1);

    /* Read back and verify exact payload */
    const void *data = NULL;
    uint32_t len = 0;
    uint64_t rticket = 0;
    assert(rbipc_read_acquire(ring, &data, &len, &rticket) == RBIPC_OK);
    assert(len == 512);
    assert(rticket == 1);
    assert(memcmp(data, payload, 512) == 0);
    assert(rbipc_read_release(ring, rticket) == RBIPC_OK);

    rbipc_detach(ring);
}

int main(void) {
    printf("[test_unit_stream_unaligned] Running unaligned streaming copy tests...\n");
    test_streaming_defensive_nulls();
    test_streaming_exhaustive_sizes();
    test_write_stream_lifecycle();
    printf("[test_unit_stream_unaligned] All tests passed successfully.\n");
    return 0;
}
