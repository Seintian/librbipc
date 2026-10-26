/**
 * @file rbipc_math.h
 * @brief Discrete mathematics, alignment geometry, and modular sequence
 * arithmetic.
 *
 * @details Implements bitwise power-of-two rounding, address alignment, and
 * modulo-2^32 sequence difference calculations based on RFC 1982 serial number
 * arithmetic.
 *
 * @author Seintian
 * @date 2026
 * @copyright Apache License 2.0
 */

#ifndef RBIPC_MATH_H
#define RBIPC_MATH_H

#include "rbipc.h"
#include "rbipc_attr.h"

#include <stddef.h>
#include <stdint.h>

/**
 * @def RBIPC_MAX_POW2_32
 * @brief Maximum 32-bit power-of-two integer ceiling (2^31).
 */
#define RBIPC_MAX_POW2_32 0x80000000U

/**
 * @def RBIPC_MAX_POW2_64
 * @brief Maximum 64-bit power-of-two integer ceiling (2^63).
 */
#define RBIPC_MAX_POW2_64 0x8000000000000000ULL

/**
 * @brief Round an unsigned 32-bit integer up to the nearest power of two.
 *
 * @details Emits branch-free bit-smearing operations:
 * \f[
 *   v' = v - 1, \quad v' \leftarrow v' \mid (v' \gg 1), \dots, \quad v'
 * \leftarrow v' \mid (v' \gg 16), \quad \text{result} = v' + 1
 * \f]
 * Safely guards against integer overflow when @p v exceeds \f$2^{31}\f$ (@ref
 * RBIPC_MAX_POW2_32).
 *
 * @param[in] v Unsigned 32-bit integer input.
 *
 * @return Nearest power of two greater than or equal to @p v.
 * @retval 1 if @p v is 0.
 * @retval 0 if @p v exceeds @ref RBIPC_MAX_POW2_32 (overflow).
 */
RBIPC_INLINE RBIPC_CONST RBIPC_NODISCARD uint32_t
rbipc_round_up_pow2_32(uint32_t v) {
  if (RBIPC_UNLIKELY(v > RBIPC_MAX_POW2_32)) {
    return 0; /* Overflow */
  }

#if defined(__GNUC__) || defined(__clang__)
  /*
   * Microarchitectural Optimization: Branchless Conditional Selection (CMOV).
   *
   * On superscalar pipelines (x86-64 / ARM64), evaluating `if (v <= 1U)` via a conditional
   * jump induces a branch instruction. Because queue capacity configuration evaluates
   * infrequently with inputs > 1, a branch predictor miss incurs a 14-20 cycle pipeline flush.
   * Conversely, `__builtin_clz(0)` triggers hardware undefined behavior (BSR flag undefinedness).
   *
   * To achieve deterministic, branchless instruction flow, the operand passed to `clz` is clamped
   * to a non-zero floor using a predicated select: `sub = (v > 1U) ? (v - 1U) : 1U`.
   * The compiler lowers this sequence to a decrement, LZCNT/BSR, and a CMOVNB instruction,
   * completely eliminating conditional jumps from the execution stream.
   *
   * Reference:
   * - Agner Fog (2024), "Optimizing Software in C++", §14.2 (Branchless conditionals).
   * - Daniel Lemire (2019), "The Cost of Branches", Software: Practice and Experience.
   */
  uint32_t sub = (v > 1U) ? (v - 1U) : 1U;
  uint32_t clz = (uint32_t)__builtin_clz(sub);
  uint32_t res = 1U << (32U - clz);
  return (v <= 1U) ? 1U : res;
#else
  v--;
  v |= v >> 1U;
  v |= v >> 2U;
  v |= v >> 4U;
  v |= v >> 8U;
  v |= v >> 16U;
  return v + 1;
#endif
}

/**
 * @brief Round an unsigned 64-bit integer up to the nearest power of two.
 *
 * @details Emits 64-bit hardware-accelerated count-leading-zeros instruction
 * (LZCNT/BSR on x86-64, CLZ on ARM64) with branchless conditional selection (CMOV)
 * on non-overflow paths.
 *
 * @param[in] v Unsigned 64-bit integer input.
 *
 * @return Nearest power of two greater than or equal to @p v.
 * @retval 1 if @p v is 0 or 1.
 * @retval 0 if @p v exceeds @ref RBIPC_MAX_POW2_64 (overflow).
 */
RBIPC_INLINE RBIPC_CONST RBIPC_NODISCARD uint64_t
rbipc_round_up_pow2_64(uint64_t v) {
  if (RBIPC_UNLIKELY(v > RBIPC_MAX_POW2_64)) {
    return 0; /* Overflow */
  }

#if defined(__GNUC__) || defined(__clang__)
  /* Branchless conditional operand clamping to prevent __builtin_clzll(0) undefined behavior */
  uint64_t sub = (v > 1ULL) ? (v - 1ULL) : 1ULL;
  uint32_t clz = (uint32_t)__builtin_clzll(sub);
  uint64_t res = 1ULL << (64U - clz);
  return (v <= 1ULL) ? 1ULL : res;
#else
  v--;
  v |= v >> 1ULL;
  v |= v >> 2ULL;
  v |= v >> 4ULL;
  v |= v >> 8ULL;
  v |= v >> 16ULL;
  v |= v >> 32ULL;
  return v + 1;
#endif
}

/**
 * @brief Align an integer value up to the next power-of-two boundary.
 *
 * @details Utilizes two's complement bitwise masking:
 * \f[
 *   \text{aligned} = (val + alignment - 1) \ \& \ \sim(alignment - 1)
 * \f]
 * Emits an invariant assumption (@ref RBIPC_ASSUME) asserting that @p alignment is strictly
 * a non-zero power of two, enabling the backend compiler to eliminate sign-extension checks
 * and simplify bitwise masking sequences.
 *
 * @param[in] val       Base integer value to align.
 * @param[in] alignment Power-of-two alignment boundary (e.g. 64 for cache line, 4096 for page).
 *
 * @return Value rounded up to the nearest multiple of @p alignment.
 */
RBIPC_INLINE RBIPC_CONST RBIPC_NODISCARD size_t
rbipc_align_up(size_t val, size_t alignment) {
  RBIPC_ASSUME(alignment > 0 && (alignment & (alignment - 1)) == 0);
  return (val + alignment - 1) & ~(alignment - 1);
}

/**
 * @brief Calculate the signed difference between two 64-bit sequence numbers
 * under modular arithmetic.
 *
 * @details Adheres to RFC 1982 Serial Number Arithmetic rules. Correctly
 * handles unsigned 64-bit overflow and wrap-around as long as the absolute
 * difference is strictly less than \f$2^{63}\f$:
 * \f[
 *   \Delta = (\text{int64\_t})(a - b)
 * \f]
 *
 * @param[in] a Monotonic sequence number A.
 * @param[in] b Monotonic sequence number B.
 *
 * @return Signed 64-bit integer representing the distance from @p b to @p a.
 *         A positive value indicates @p a is ahead of @p b; negative indicates
 * @p a is behind @p b.
 */
RBIPC_INLINE RBIPC_CONST RBIPC_NODISCARD int64_t rbipc_seq_diff(uint64_t a,
                                                                uint64_t b) {
  return (int64_t)(a - b);
}


/**
 * @brief Test if an unsigned 32-bit integer is strictly a non-zero power of two.
 *
 * @param[in] v Integer to test.
 * @return True if @p v is a power of two, false otherwise.
 */
RBIPC_INLINE RBIPC_CONST RBIPC_NODISCARD bool rbipc_is_pow2_32(uint32_t v) {
  return (v > 0) && ((v & (v - 1U)) == 0);
}

/**
 * @brief Test if an unsigned 64-bit integer is strictly a non-zero power of two.
 *
 * @param[in] v Integer to test.
 * @return True if @p v is a power of two, false otherwise.
 */
RBIPC_INLINE RBIPC_CONST RBIPC_NODISCARD bool rbipc_is_pow2_64(uint64_t v) {
  return (v > 0) && ((v & (v - 1ULL)) == 0);
}

/**
 * @brief Count number of set bits (population count) in a 32-bit integer.
 *
 * @param[in] v 32-bit integer.
 * @return Count of 1 bits.
 */
RBIPC_INLINE RBIPC_CONST RBIPC_NODISCARD uint32_t rbipc_popcount_32(uint32_t v) {
#if defined(__GNUC__) || defined(__clang__)
  return (uint32_t)__builtin_popcount(v);
#else
  v = v - ((v >> 1U) & 0x55555555U);
  v = (v & 0x33333333U) + ((v >> 2U) & 0x33333333U);
  return (((v + (v >> 4U)) & 0x0F0F0F0FU) * 0x01010101U) >> 24U;
#endif
}

/**
 * @brief Count number of set bits (population count) in a 64-bit integer.
 *
 * @param[in] v 64-bit integer.
 * @return Count of 1 bits.
 */
RBIPC_INLINE RBIPC_CONST RBIPC_NODISCARD uint32_t rbipc_popcount_64(uint64_t v) {
#if defined(__GNUC__) || defined(__clang__)
  return (uint32_t)__builtin_popcountll(v);
#else
  v = v - ((v >> 1ULL) & 0x5555555555555555ULL);
  v = (v & 0x3333333333333333ULL) + ((v >> 2ULL) & 0x3333333333333333ULL);
  return (uint32_t)((((v + (v >> 4ULL)) & 0x0F0F0F0F0F0F0F0FULL) * 0x0101010101010101ULL) >> 56ULL);
#endif
}

/**
 * @brief Count trailing zeros in a 32-bit integer.
 *
 * @details Emits an invariant assumption (@ref RBIPC_ASSUME) asserting that @p v is non-zero.
 * On x86-64 and ARM architectures, hardware trailing zero count instructions (TZCNT or BSF/RBIT)
 * exhibit undefined result flags when the operand is zero. Communicating this precondition to the
 * optimizer suppresses redundant zero-check branches and enables downstream range propagation.
 *
 * @param[in] v Non-zero 32-bit integer. Precondition: @p v != 0.
 * @return Number of trailing zero bits in the range [0, 31].
 */
RBIPC_INLINE RBIPC_CONST RBIPC_NODISCARD uint32_t rbipc_ctz_32(uint32_t v) {
  RBIPC_ASSUME(v != 0);
#if defined(__GNUC__) || defined(__clang__)
  return (uint32_t)__builtin_ctz(v);
#else
  return rbipc_popcount_32((v & -v) - 1U);
#endif
}

/**
 * @brief Count trailing zeros in a 64-bit integer.
 *
 * @details Emits an invariant assumption (@ref RBIPC_ASSUME) asserting that @p v is non-zero.
 *
 * @param[in] v Non-zero 64-bit integer. Precondition: @p v != 0.
 * @return Number of trailing zero bits in the range [0, 63].
 */
RBIPC_INLINE RBIPC_CONST RBIPC_NODISCARD uint32_t rbipc_ctz_64(uint64_t v) {
  RBIPC_ASSUME(v != 0);
#if defined(__GNUC__) || defined(__clang__)
  return (uint32_t)__builtin_ctzll(v);
#else
  return rbipc_popcount_64((v & -v) - 1ULL);
#endif
}

#endif /* RBIPC_MATH_H */
