/**
 * @file rbipc_math.h
 * @brief Integer arithmetic, alignment, and sequence math helpers
 */

#ifndef RBIPC_MATH_H
#define RBIPC_MATH_H

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Round up a 32-bit unsigned integer to the nearest power of two.
 * Returns 0 on overflow (e.g. if v > 0x80000000).
 */
static inline uint32_t rbipc_round_up_pow2_32(uint32_t v) {
    if (v == 0) return 1;
    if (v > 0x80000000U) return 0; /* Overflow */
    v--;
    v |= v >> 1U;
    v |= v >> 2U;
    v |= v >> 4U;
    v |= v >> 8U;
    v |= v >> 16U;
    return v + 1;
}

/**
 * @brief Align value up to the specified power-of-two alignment boundary.
 */
static inline size_t rbipc_align_up(size_t val, size_t alignment) {
    return (val + alignment - 1) & ~(alignment - 1);
}

/**
 * @brief Signed difference between two 32-bit sequence numbers.
 * Handles modulo-2^32 wrap-around correctly as long as difference < 2^31.
 */
static inline int32_t rbipc_seq_diff(uint32_t a, uint32_t b) {
    return (int32_t)(a - b);
}

#endif /* RBIPC_MATH_H */
