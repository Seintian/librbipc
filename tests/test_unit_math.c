/**
 * @file test_unit_math.c
 * @brief Unit tests for math, alignment, and sequence distance helpers
 */

#include "rbipc.h"
#include "../src/rbipc_math.h"
#include <stdio.h>
#include <assert.h>
#include <limits.h>

static void test_round_up_pow2(void) {
    assert(rbipc_round_up_pow2_32(0) == 1);
    assert(rbipc_round_up_pow2_32(1) == 1);
    assert(rbipc_round_up_pow2_32(2) == 2);
    assert(rbipc_round_up_pow2_32(3) == 4);
    assert(rbipc_round_up_pow2_32(4) == 4);
    assert(rbipc_round_up_pow2_32(5) == 8);
    assert(rbipc_round_up_pow2_32(7) == 8);
    assert(rbipc_round_up_pow2_32(8) == 8);
    assert(rbipc_round_up_pow2_32(9) == 16);
    assert(rbipc_round_up_pow2_32(1023) == 1024);
    assert(rbipc_round_up_pow2_32(1024) == 1024);
    assert(rbipc_round_up_pow2_32(1025) == 2048);
    assert(rbipc_round_up_pow2_32(0x40000000U) == 0x40000000U);
    assert(rbipc_round_up_pow2_32(0x7FFFFFFFU) == 0x80000000U);
    assert(rbipc_round_up_pow2_32(0x80000000U) == 0x80000000U);
    /* Overflow cases */
    assert(rbipc_round_up_pow2_32(0x80000001U) == 0);
    assert(rbipc_round_up_pow2_32(UINT32_MAX) == 0);
}

static void test_align_up(void) {
    assert(rbipc_align_up(0, 64) == 0);
    assert(rbipc_align_up(1, 64) == 64);
    assert(rbipc_align_up(63, 64) == 64);
    assert(rbipc_align_up(64, 64) == 64);
    assert(rbipc_align_up(65, 64) == 128);

    assert(rbipc_align_up(4095, 4096) == 4096);
    assert(rbipc_align_up(4096, 4096) == 4096);
    assert(rbipc_align_up(4097, 4096) == 8192);
}

static void test_seq_diff(void) {
    assert(rbipc_seq_diff(0, 0) == 0);
    assert(rbipc_seq_diff(10, 5) == 5);
    assert(rbipc_seq_diff(5, 10) == -5);

    /* Wrap-around testing: a is just after wrap, b is just before wrap */
    uint32_t b = UINT32_MAX - 2;
    uint32_t a = 2; /* 5 steps ahead after wrap */
    assert(rbipc_seq_diff(a, b) == 5);
    assert(rbipc_seq_diff(b, a) == -5);

    assert(rbipc_seq_diff(0, UINT32_MAX) == 1);
    assert(rbipc_seq_diff(UINT32_MAX, 0) == -1);
}

int main(void) {
    printf("[test_unit_math] Running tests...\n");
    test_round_up_pow2();
    test_align_up();
    test_seq_diff();
    printf("[test_unit_math] All tests passed!\n");
    return 0;
}
