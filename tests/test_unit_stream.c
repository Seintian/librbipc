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

    printf("[test_unit_stream] All tests passed!\n");
    return 0;
}