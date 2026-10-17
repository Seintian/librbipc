/**
 * @file test_unit_simd_parity.c
 * @brief Unit tests for multi-architecture SIMD sequence scanning parity,
 *        sub-vector batching (SSE4.1/AVX2/scalar), and boundary wrap-around.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "rbipc.h"
#include "rbipc_simd.h"
#include "rbipc_slot.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static void test_simd_small_counts(void) {
    const uint32_t cap = 32;
    const uint32_t mask = cap - 1;
    rbipc_slot_t slots[32];
    rbipc_slot_init_table(slots, cap);

    /* Test counts 1, 2, 3, 5, 7 across multiple ticket offsets */
    const uint32_t counts[] = {1, 2, 3, 4, 5, 6, 7, 8, 11, 15, 16, 24, 31, 32};
    const size_t num_counts = sizeof(counts) / sizeof(counts[0]);

    for (uint64_t base_t = 0; base_t < 64; base_t += 7) {
        /* Set all slots up to base_t + 32 to matching vacant sequence */
        for (uint32_t i = 0; i < cap; ++i) {
            uint64_t seq = base_t + i;
            atomic_store_explicit(&slots[(base_t + i) & mask].sequence, seq, memory_order_relaxed);
        }

        for (size_t c = 0; c < num_counts; ++c) {
            uint32_t count = counts[c];
            uint32_t avail = rbipc_simd_scan_vacant_slots(slots, mask, base_t, count);
            assert(avail == count);

            /* Inject mismatch at each position from 0 to count - 1 */
            for (uint32_t m = 0; m < count; ++m) {
                uint64_t orig = atomic_load_explicit(&slots[(base_t + m) & mask].sequence, memory_order_relaxed);
                atomic_store_explicit(&slots[(base_t + m) & mask].sequence, orig + 9999, memory_order_relaxed);

                uint32_t partial = rbipc_simd_scan_vacant_slots(slots, mask, base_t, count);
                assert(partial == m);

                /* Restore original sequence */
                atomic_store_explicit(&slots[(base_t + m) & mask].sequence, orig, memory_order_relaxed);
            }
        }
    }
}

static void test_simd_ready_small_counts(void) {
    const uint32_t cap = 32;
    const uint32_t mask = cap - 1;
    rbipc_slot_t slots[32];
    rbipc_slot_init_table(slots, cap);

    const uint32_t counts[] = {1, 2, 3, 4, 5, 6, 7, 8, 12, 16};
    const size_t num_counts = sizeof(counts) / sizeof(counts[0]);

    for (uint64_t base_t = 0; base_t < 48; base_t += 5) {
        /* Mark ready: seq = ticket + 1 */
        for (uint32_t i = 0; i < cap; ++i) {
            uint64_t seq = base_t + i + 1;
            atomic_store_explicit(&slots[(base_t + i) & mask].sequence, seq, memory_order_relaxed);
        }

        for (size_t c = 0; c < num_counts; ++c) {
            uint32_t count = counts[c];
            uint32_t ready = rbipc_simd_scan_ready_slots(slots, mask, base_t, count);
            assert(ready == count);

            /* Inject mismatch at each position */
            for (uint32_t m = 0; m < count; ++m) {
                uint64_t orig = atomic_load_explicit(&slots[(base_t + m) & mask].sequence, memory_order_relaxed);
                atomic_store_explicit(&slots[(base_t + m) & mask].sequence, orig + 1234, memory_order_relaxed);

                uint32_t partial = rbipc_simd_scan_ready_slots(slots, mask, base_t, count);
                assert(partial == m);

                atomic_store_explicit(&slots[(base_t + m) & mask].sequence, orig, memory_order_relaxed);
            }
        }
    }
}

static void test_simd_boundary_wrap(void) {
    const uint32_t cap = 16;
    const uint32_t mask = cap - 1;
    rbipc_slot_t slots[16];
    rbipc_slot_init_table(slots, cap);

    /* Ticket starts at 14, scanning 6 slots (spanning slots 14, 15, 0, 1, 2, 3) */
    uint64_t t = 14;
    for (uint32_t i = 0; i < 6; ++i) {
        atomic_store_explicit(&slots[(t + i) & mask].sequence, t + i, memory_order_relaxed);
    }

    assert(rbipc_simd_scan_vacant_slots(slots, mask, t, 6) == 6);

    /* Mismatch on wrapped slot 1 (offset 3) */
    atomic_store_explicit(&slots[(t + 3) & mask].sequence, 0xDEADBEEF, memory_order_relaxed);
    assert(rbipc_simd_scan_vacant_slots(slots, mask, t, 6) == 3);

    /* Restore */
    atomic_store_explicit(&slots[(t + 3) & mask].sequence, t + 3, memory_order_relaxed);
    assert(rbipc_simd_scan_vacant_slots(slots, mask, t, 6) == 6);
}

int main(void) {
    printf("[test_unit_simd_parity] Running SIMD sequence scanning parity tests...\n");
    test_simd_small_counts();
    test_simd_ready_small_counts();
    test_simd_boundary_wrap();
    printf("[test_unit_simd_parity] All tests passed!\n");
    return 0;
}