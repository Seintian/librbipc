/**
 * @file deep_simd.c
 * @brief SIMD (AVX2) sequence scanning vs scalar iteration micro-benchmark
 */

#include "deep_common.h"

#if defined(__AVX2__)
#include <immintrin.h>
#endif

void run_simd_scan_bench(int iterations, simd_scan_bench_t *out) {
    rbipc_destroy(DEEP_SHM_A);
    rbipc_ring_t *ring = NULL;
    assert(rbipc_create(DEEP_SHM_A, 1024, 64, &ring) == RBIPC_OK);

    uint32_t mask = ring->hdr->capacity_mask;
    rbipc_slot_t *slots = (rbipc_slot_t *)ring->slots;

    /* Populate slots as ready for reading */
    for (uint32_t i = 0; i < 1024; ++i) {
        atomic_store_explicit(&slots[i].sequence, i + 1, memory_order_relaxed);
    }

    uint32_t batch_size = 32;

    /* 1. Scalar scan measurement */
    uint64_t t0 = get_time_ns();
    uint64_t scalar_total = 0;
    for (int it = 0; it < iterations; ++it) {
        uint64_t t = (uint64_t)(it % 512);
        uint32_t avail = 0;
        for (uint32_t i = 0; i < batch_size; ++i) {
            uint64_t seq = atomic_load_explicit(&slots[(t + i) & mask].sequence, memory_order_acquire);
            if (seq == t + i + 1) {
                avail++;
            } else {
                break;
            }
        }
        scalar_total += avail;
    }
    uint64_t t1 = get_time_ns();

    /* 2. Vectorized SIMD scan measurement */
    uint64_t t2 = get_time_ns();
    uint64_t simd_total = 0;
    for (int it = 0; it < iterations; ++it) {
        uint64_t t = (uint64_t)(it % 512);
        uint32_t avail = rbipc_simd_scan_ready_slots(slots, mask, t, batch_size);
        simd_total += avail;
    }
    uint64_t t3 = get_time_ns();

    assert(scalar_total == simd_total);

    double scalar_ns = (double)(t1 - t0) / (double)iterations;
    double simd_ns   = (double)(t3 - t2) / (double)iterations;

    out->scalar_ns_per_batch = scalar_ns;
    out->simd_ns_per_batch   = simd_ns;
    out->speedup_pct         = ((scalar_ns - simd_ns) / scalar_ns) * 100.0;

    rbipc_detach(ring);
    rbipc_destroy(DEEP_SHM_A);
}
