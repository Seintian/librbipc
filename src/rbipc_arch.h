/**
 * @file rbipc_arch.h
 * @brief Architecture-specific intrinsics and timing primitives for librbipc
 */

#ifndef RBIPC_ARCH_H
#define RBIPC_ARCH_H

#include <stdint.h>
#include <time.h>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
static inline void rbipc_cpu_pause(void) {
    _mm_pause();
}
#elif defined(__aarch64__) || defined(__arm__)
static inline void rbipc_cpu_pause(void) {
    __asm__ __volatile__("isb\n" ::: "memory");
}
#elif defined(__riscv)
static inline void rbipc_cpu_pause(void) {
    __asm__ __volatile__("pause\n" ::: "memory");
}
#else
static inline void rbipc_cpu_pause(void) {
    __asm__ __volatile__("" ::: "memory");
}
#endif

/**
 * @brief Get high-resolution monotonic time in nanoseconds.
 */
static inline uint64_t rbipc_clock_monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
}

#endif /* RBIPC_ARCH_H */
