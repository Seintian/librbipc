/**
 * @file rbipc_arch.h
 * @brief Architecture-specific hardware intrinsics and high-resolution timing primitives.
 *
 * @details Encapsulates CPU pipeline pause instructions across x86_64, AArch64, ARM, and RISC-V
 * architectures to optimize memory ordering pipeline stalls during spin-wait phases and prevent
 * speculative pipeline thrashing, alongside POSIX CLOCK_MONOTONIC timing wrappers.
 *
 * @author Christian Santarelli
 * @date 2026
 * @copyright Apache License 2.0
 */

#ifndef RBIPC_ARCH_H
#define RBIPC_ARCH_H

#include "rbipc.h"
#include "rbipc_attr.h"

#include <stdint.h>
#include <time.h>

/**
 * @brief Emit a hardware processor hint indicating an execution pause or spin-wait loop.
 *
 * @details
 * - On x86/x86_64: Emits `_mm_pause()` (PAUSE instruction), de-pipelining memory order violations.
 * - On AArch64/ARM: Emits `isb` (Instruction Synchronization Barrier) to flush speculative fetch stages.
 * - On RISC-V: Emits `pause` instruction.
 * - Fallback: Emits a compiler memory clobber barrier without hardware instruction.
 *
 * @note In librbipc passive zero-spin mode, execution suspends via sys_futex rather than spinning.
 */
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
RBIPC_INLINE void rbipc_cpu_pause(void) {
    _mm_pause();
}
#elif defined(__aarch64__) || defined(__arm__)
RBIPC_INLINE void rbipc_cpu_pause(void) {
    __asm__ __volatile__("isb\n" ::: "memory");
}
#elif defined(__riscv)
RBIPC_INLINE void rbipc_cpu_pause(void) {
    __asm__ __volatile__("pause\n" ::: "memory");
}
#else
RBIPC_INLINE void rbipc_cpu_pause(void) {
    __asm__ __volatile__("" ::: "memory");
}
#endif

/**
 * @brief Query current high-resolution monotonic clock timestamp in nanoseconds.
 *
 * @details Reads Linux POSIX CLOCK_MONOTONIC clock source. Monotonic timestamps are strictly
 * immune to wall-clock time adjustments, NTP skews, and leap-second discontinuities, making
 * them ideal for deadline calculations and performance latency profiling.
 *
 * @return 64-bit integer timestamp in nanoseconds.
 */
RBIPC_INLINE RBIPC_NODISCARD uint64_t rbipc_clock_monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * RBIPC_NSEC_PER_SEC) + (uint64_t)ts.tv_nsec;
}

#endif /* RBIPC_ARCH_H */
