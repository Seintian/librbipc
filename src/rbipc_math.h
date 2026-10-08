/**
 * @file rbipc_math.h
 * @brief Discrete mathematics, alignment geometry, and modular sequence arithmetic.
 *
 * @details Implements bitwise power-of-two rounding, address alignment, and modulo-2^32
 * sequence difference calculations based on RFC 1982 serial number arithmetic.
 *
 * @author Christian Santarelli
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
 * @brief Round an unsigned 32-bit integer up to the nearest power of two.
 *
 * @details Emits branch-free bit-smearing operations:
 * \f[
 *   v' = v - 1, \quad v' \leftarrow v' \mid (v' \gg 1), \dots, \quad v' \leftarrow v' \mid (v' \gg 16), \quad \text{result} = v' + 1
 * \f]
 * Safely guards against integer overflow when @p v exceeds \f$2^{31}\f$ (@ref RBIPC_MAX_POW2_32).
 *
 * @param[in] v Unsigned 32-bit integer input.
 *
 * @return Nearest power of two greater than or equal to @p v.
 * @retval 1 if @p v is 0.
 * @retval 0 if @p v exceeds @ref RBIPC_MAX_POW2_32 (overflow).
 */
RBIPC_INLINE RBIPC_CONST RBIPC_NODISCARD uint32_t rbipc_round_up_pow2_32(uint32_t v) {
    if (v == 0) return 1;
    if (v > RBIPC_MAX_POW2_32) return 0; /* Overflow */

    v--;
    v |= v >> 1U;
    v |= v >> 2U;
    v |= v >> 4U;
    v |= v >> 8U;
    v |= v >> 16U;
    return v + 1;
}

/**
 * @brief Align an integer value up to the next power-of-two boundary.
 *
 * @details Utilizes two's complement bitwise masking:
 * \f[
 *   \text{aligned} = (val + alignment - 1) \ \& \ \sim(alignment - 1)
 * \f]
 *
 * @param[in] val       Base integer value to align.
 * @param[in] alignment Power-of-two alignment boundary (e.g. 64 for cache line, 4096 for page).
 *
 * @return Value rounded up to the nearest multiple of @p alignment.
 */
RBIPC_INLINE RBIPC_CONST RBIPC_NODISCARD size_t rbipc_align_up(size_t val, size_t alignment) {
    return (val + alignment - 1) & ~(alignment - 1);
}

/**
 * @brief Calculate the signed difference between two 32-bit sequence numbers under modular arithmetic.
 *
 * @details Adheres to RFC 1982 Serial Number Arithmetic rules. Correctly handles unsigned
 * 32-bit overflow and wrap-around as long as the absolute difference is strictly less than \f$2^{31}\f$:
 * \f[
 *   \Delta = (\text{int32\_t})(a - b)
 * \f]
 *
 * @param[in] a Monotonic sequence number A.
 * @param[in] b Monotonic sequence number B.
 *
 * @return Signed 32-bit integer representing the distance from @p b to @p a.
 *         A positive value indicates @p a is ahead of @p b; negative indicates @p a is behind @p b.
 */
RBIPC_INLINE RBIPC_CONST RBIPC_NODISCARD int32_t rbipc_seq_diff(uint32_t a, uint32_t b) {
    return (int32_t)(a - b);
}

#endif /* RBIPC_MATH_H */
