/**
 * @file opt_simd.c
 * @brief Module 6: SIMD Sequence Scanning Engines (Scalar, SSE4.1, AVX1 Fallback, AVX2).
 */

#include "arch_common.h"

void run_module6_simd(uint64_t scan_iterations, m6_simd_result_t results[4]) {
    arch_print_subhdr("Module 6: SIMD Sequence Scanning Engines");
    printf("Iterations: %lu (Total Slots Evaluated: %lu) | 256-Slot Ring Array\n\n",
           (unsigned long)scan_iterations, (unsigned long)(scan_iterations * 4));

    const uint32_t cap = 256;
    const uint32_t mask = cap - 1;
    alignas(64) rbipc_slot_t slots[256];
    rbipc_slot_init_table(slots, cap);

    results[0].engine_name = "Scalar Baseline";
    results[1].engine_name = "SSE4.1 (128-bit)";
    results[2].engine_name = "AVX1 Fallback (256-bit)";
    results[3].engine_name = "AVX2 Vectorized (256-bit)";

    const uint64_t total_slots = scan_iterations * 4;

    for (int is_packed = 0; is_packed <= 1; ++is_packed) {
        bool packed = (bool)is_packed;

        /* Populate slots as matching vacant */
        for (uint32_t i = 0; i < cap; ++i) {
            uint64_t seq = (uint64_t)i;
            uint64_t raw = packed ? (seq << RBIPC_PACKED_SEQ_SHIFT) : seq;
            atomic_store_explicit(&slots[i].sequence, raw, memory_order_relaxed);
        }

        /* 1. Scalar */
        uint64_t t0 = arch_get_time_ns();
        uint64_t sink0 = 0;
        for (uint64_t it = 0; it < scan_iterations; ++it) {
            uint64_t t = (it * 4) & mask;
            sink0 += rbipc_simd_scan_scalar(slots, mask, t, 4, 0, packed);
        }
        uint64_t t1 = arch_get_time_ns();
        __asm__ volatile("" : "+r"(sink0));
        double sec0 = (double)(t1 - t0) / 1e9;
        double mslots0 = ((double)total_slots / 1e6) / sec0;

        /* 2. SSE4.1 */
        double mslots1 = 0.0;
        if (__builtin_cpu_supports("sse4.1")) {
            uint64_t t2 = arch_get_time_ns();
            uint64_t sink1 = 0;
            for (uint64_t it = 0; it < scan_iterations; ++it) {
                uint64_t t = (it * 4) & mask;
                sink1 += rbipc_simd_scan_sse41(slots, mask, t, 4, 0, packed);
            }
            uint64_t t3 = arch_get_time_ns();
            __asm__ volatile("" : "+r"(sink1));
            mslots1 = ((double)total_slots / 1e6) / ((double)(t3 - t2) / 1e9);
        }

        /* 3. AVX1 Fallback */
        double mslots2 = 0.0;
        if (__builtin_cpu_supports("avx")) {
            uint64_t t4 = arch_get_time_ns();
            uint64_t sink2 = 0;
            for (uint64_t it = 0; it < scan_iterations; ++it) {
                uint64_t t = (it * 4) & mask;
                sink2 += rbipc_simd_scan_avx(slots, mask, t, 4, 0, packed);
            }
            uint64_t t5 = arch_get_time_ns();
            __asm__ volatile("" : "+r"(sink2));
            mslots2 = ((double)total_slots / 1e6) / ((double)(t5 - t4) / 1e9);
        }

        /* 4. AVX2 */
        double mslots3 = 0.0;
        if (__builtin_cpu_supports("avx2")) {
            uint64_t t6 = arch_get_time_ns();
            uint64_t sink3 = 0;
            for (uint64_t it = 0; it < scan_iterations; ++it) {
                uint64_t t = (it * 4) & mask;
                sink3 += rbipc_simd_scan_avx2(slots, mask, t, 4, 0, packed);
            }
            uint64_t t7 = arch_get_time_ns();
            __asm__ volatile("" : "+r"(sink3));
            mslots3 = ((double)total_slots / 1e6) / ((double)(t7 - t6) / 1e9);
        }

        if (!packed) {
            results[0].std_mslots_sec = mslots0;
            results[0].std_speedup = 1.0;
            results[1].std_mslots_sec = mslots1;
            results[1].std_speedup = mslots1 / mslots0;
            results[2].std_mslots_sec = mslots2;
            results[2].std_speedup = mslots2 / mslots0;
            results[3].std_mslots_sec = mslots3;
            results[3].std_speedup = mslots3 / mslots0;
        } else {
            results[0].packed_mslots_sec = mslots0;
            results[0].packed_speedup = 1.0;
            results[1].packed_mslots_sec = mslots1;
            results[1].packed_speedup = mslots1 / mslots0;
            results[2].packed_mslots_sec = mslots2;
            results[2].packed_speedup = mslots2 / mslots0;
            results[3].packed_mslots_sec = mslots3;
            results[3].packed_speedup = mslots3 / mslots0;
        }
    }

    for (int e = 0; e < 4; ++e) {
        printf("  %-25s : Standard 64-bit: %7.2f Mslots/s [%4.2fx] | Tier 3 Packed: %7.2f Mslots/s [%4.2fx]\n",
               results[e].engine_name,
               results[e].std_mslots_sec, results[e].std_speedup,
               results[e].packed_mslots_sec, results[e].packed_speedup);
    }
    printf("\n");
}
