/**
 * @file rbipc_attr.h
 * @brief Internal compiler annotations, optimization attributes, and architecture builtins.
 *
 * @details Provides compiler abstraction macros targeting GCC and Clang compilers to enable
 * aggressive inlining, stack frame flattening, hardware cache prefetching, and compiler-guided
 * control-flow optimization without introducing compiler vendor lock-in.
 *
 * @author Christian Santarelli
 * @date 2026
 * @copyright Apache License 2.0
 */

#ifndef RBIPC_ATTR_H
#define RBIPC_ATTR_H

#include "rbipc.h"

#if defined(__GNUC__) || defined(__clang__)

/**
 * @def RBIPC_INLINE
 * @brief Forces the compiler to inline the annotated subroutine into all caller sites.
 * @details Combines C99 static inline semantics with `__attribute__((always_inline))`.
 * Eliminates function call overhead (call/ret instructions, parameter shuffling) on the hot path.
 */
#define RBIPC_INLINE            static inline __attribute__((always_inline))

/**
 * @defgroup rbipc_prefetch Prefetch Parameters
 * @brief Memory access and cache hints for hardware prefetching.
 * @{
 */
#define RBIPC_PREFETCH_READ             0 /**< Hardware prefetch for read access. */
#define RBIPC_PREFETCH_WRITE            1 /**< Hardware prefetch for write access. */
#define RBIPC_PREFETCH_LOCALITY_NONE    0 /**< No temporal cache locality. */
#define RBIPC_PREFETCH_LOCALITY_LOW     1 /**< Low temporal cache locality. */
#define RBIPC_PREFETCH_LOCALITY_MED     2 /**< Moderate temporal cache locality. */
#define RBIPC_PREFETCH_LOCALITY_HIGH    3 /**< High temporal cache locality (retain in L1 cache). */
/** @} */

/**
 * @def RBIPC_PREFETCH(addr, rw, loc)
 * @brief Emits hardware prefetch instructions to prime the processor cache hierarchy.
 *
 * @param addr Address of memory buffer or structure to prefetch.
 * @param rw   Prefetch intention: @ref RBIPC_PREFETCH_READ or @ref RBIPC_PREFETCH_WRITE.
 * @param loc  Temporal locality level (e.g. @ref RBIPC_PREFETCH_LOCALITY_HIGH).
 */
#define RBIPC_PREFETCH(addr, rw, loc) __builtin_prefetch((addr), (rw), (loc))

#else
#define RBIPC_INLINE            static inline
#define RBIPC_PREFETCH_READ             0
#define RBIPC_PREFETCH_WRITE            1
#define RBIPC_PREFETCH_LOCALITY_NONE    0
#define RBIPC_PREFETCH_LOCALITY_LOW     1
#define RBIPC_PREFETCH_LOCALITY_MED     2
#define RBIPC_PREFETCH_LOCALITY_HIGH    3
#define RBIPC_PREFETCH(addr, rw, loc) ((void)0)
#endif

#endif /* RBIPC_ATTR_H */
