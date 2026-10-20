/**
 * @file test_unit_simd.c
 * @brief Unit tests for SIMD sequence scanning engine.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "rbipc.h"
#include "rbipc_simd.h"
#include "rbipc_slot.h"

#include <assert.h>
#include <stdio.h>

int main(void) {
    printf("[test_unit_simd] Running SIMD sequence scanning tests...\n");

    const uint32_t capacity = 16;
    const uint32_t mask = capacity - 1;
    rbipc_slot_t slots[16];
    rbipc_slot_init_table(slots, capacity);

    /* 1. All vacant from ticket 0 */
    uint32_t v = rbipc_simd_scan_vacant_slots(slots, mask, 0, 16);
    assert(v == 16);

    /* 2. Partial vacant (first 6 vacant, slot 6 occupied with next sequence) */
    atomic_store_explicit(&slots[6].sequence, 999, memory_order_relaxed);
    v = rbipc_simd_scan_vacant_slots(slots, mask, 0, 16);
    assert(v == 6);

    /* 3. Wrap-around boundary scanning */
    atomic_store_explicit(&slots[6].sequence, 6, memory_order_relaxed);
    /* For tickets 16 and 17, slots 0 and 1 must advance their sequence to cycle 1 (seq = ticket) */
    atomic_store_explicit(&slots[0].sequence, 16, memory_order_relaxed);
    atomic_store_explicit(&slots[1].sequence, 17, memory_order_relaxed);
    /* Slots 14, 15, 0, 1 ... */
    v = rbipc_simd_scan_vacant_slots(slots, mask, 14, 4);
    assert(v == 4);

    /* 4. Ready slots scanning */
    rbipc_slot_init_table(slots, capacity);
    /* Mark slots 0..3 committed (seq = ticket + 1) */
    for (uint32_t i = 0; i < 4; ++i) {
        atomic_store_explicit(&slots[i].sequence, i + 1, memory_order_relaxed);
    }
    uint32_t r = rbipc_simd_scan_ready_slots(slots, mask, 0, 8);
    assert(r == 4);

    /* 5. Defensive null / zero count */
    assert(rbipc_simd_scan_vacant_slots(NULL, mask, 0, 10) == 0);
    assert(rbipc_simd_scan_vacant_slots(slots, mask, 0, 0) == 0);
    assert(rbipc_simd_scan_ready_slots(NULL, mask, 0, 10) == 0);
    assert(rbipc_simd_scan_ready_slots(slots, mask, 0, 0) == 0);

    printf("[test_unit_simd] All tests passed successfully.\n");
    return 0;
}
