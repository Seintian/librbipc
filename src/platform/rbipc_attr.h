/**
 * @file rbipc_attr.h
 * @brief Internal compiler annotations, optimization attributes, and
 * architecture builtins.
 *
 * @details Provides compiler abstraction macros targeting GCC and Clang
 * compilers to enable aggressive inlining, stack frame flattening, hardware
 * cache prefetching, and compiler-guided control-flow optimization without
 * introducing compiler vendor lock-in.
 *
 * @author Seintian
 * @date 2026
 * @copyright Apache License 2.0
 */

#ifndef RBIPC_ATTR_H
#define RBIPC_ATTR_H

#if defined(__GNUC__) || defined(__clang__)

/**
 * @def RBIPC_INTERNAL
 * @brief Hides internal function from dynamic shared library symbol exports.
 */
#ifndef RBIPC_INTERNAL
#define RBIPC_INTERNAL __attribute__((visibility("hidden")))
#endif

/**
 * @def RBIPC_INLINE
 * @brief Forces the compiler to inline the annotated subroutine into all caller sites.
 * @details Combines C99 static inline semantics with `__attribute__((always_inline))`.
 * Eliminates function call overhead (call/ret instructions, register spilling) on the hot path.
 */
#ifndef RBIPC_INLINE
#define RBIPC_INLINE static inline __attribute__((always_inline))
#endif

/**
 * @def RBIPC_NOINLINE
 * @brief Directs the compiler optimizer to inhibit inlining of the decorated subroutine.
 * @details Retains separate subroutine symbol emission for cold error paths or large recovery procedures.
 */
#ifndef RBIPC_NOINLINE
#define RBIPC_NOINLINE __attribute__((noinline))
#endif

/**
 * @def RBIPC_FLATTEN
 * @brief Directs the optimizer to inline all subroutines called by the annotated function.
 * @details Flattens the entire call hierarchy within the function body, maximizing loop vectorization
 * and cross-call instruction scheduling.
 */
#ifndef RBIPC_FLATTEN
#define RBIPC_FLATTEN __attribute__((flatten))
#endif

/**
 * @def RBIPC_NODISCARD
 * @brief Directs the compiler to emit a compilation diagnostic if the returned value is ignored.
 */
#ifndef RBIPC_NODISCARD
#define RBIPC_NODISCARD __attribute__((warn_unused_result))
#endif

/**
 * @def RBIPC_RETURNS_NONNULL
 * @brief Informs the optimizer that the function will never evaluate to a null pointer.
 */
#ifndef RBIPC_RETURNS_NONNULL
#define RBIPC_RETURNS_NONNULL __attribute__((returns_nonnull))
#endif

/**
 * @def RBIPC_PURE
 * @brief Designates a function whose return value depends exclusively on arguments and dereferenced memory.
 * @details Informs the optimizer that the subroutine performs no observable side-effects, enabling
 * Common Subexpression Elimination (CSE) across loop iterations.
 */
#ifndef RBIPC_PURE
#define RBIPC_PURE __attribute__((pure))
#endif

/**
 * @def RBIPC_CONST
 * @brief Designates a pure mathematical function examining solely its immediate value parameters.
 * @details The function cannot read mutable memory or produce side-effects, permitting aggressive
 * compile-time folding and hoisting out of loop nests.
 */
#ifndef RBIPC_CONST
#define RBIPC_CONST __attribute__((const))
#endif

/**
 * @def RBIPC_HOT
 * @brief Instructs the code generator that the function resides on the critical execution path.
 * @details Paces basic blocks into high-density code sections (.text.hot) to minimize instruction cache
 * miss rates and Branch Target Buffer (BTB) invalidations.
 */
#ifndef RBIPC_HOT
#define RBIPC_HOT __attribute__((hot))
#endif

/**
 * @def RBIPC_COLD
 * @brief Informs the compiler that execution of the function is statistically improbable.
 * @details Relegates code generation to out-of-line sections (.text.unlikely), preventing instruction
 * cache pollution along primary execution paths.
 */
#ifndef RBIPC_COLD
#define RBIPC_COLD __attribute__((cold))
#endif

/**
 * @def RBIPC_LEAF
 * @brief Asserts that the subroutine does not invoke caller callbacks or execute non-local gotos.
 * @details Optimizes global register allocation across translation unit boundaries.
 */
#ifndef RBIPC_LEAF
#define RBIPC_LEAF __attribute__((leaf))
#endif

/**
 * @def RBIPC_LIKELY(x)
 * @brief Informs the branch predictor that condition @p x evaluates to true with high statistical bias.
 * @details Implemented via `__builtin_expect(!!(x), 1)`. Reorders basic blocks to ensure the taken path
 * executes as a contiguous instruction stream without unconditional jumps.
 */
#ifndef RBIPC_LIKELY
#define RBIPC_LIKELY(x) (__builtin_expect(!!(x), 1))
#endif

/**
 * @def RBIPC_UNLIKELY(x)
 * @brief Informs the branch predictor that condition @p x evaluates to false with high statistical bias.
 * @details Implemented via `__builtin_expect(!!(x), 0)`. Partitions unlikely error paths out-of-line.
 */
#ifndef RBIPC_UNLIKELY
#define RBIPC_UNLIKELY(x) (__builtin_expect(!!(x), 0))
#endif

/**
 * @def RBIPC_ASSUME_ALIGNED(p, a)
 * @brief Informs the optimizer that memory pointer @p p satisfies byte alignment boundary @p a.
 * @details Suppresses redundant dynamic alignment checks and enables aligned SIMD load/store instructions.
 */
#ifndef RBIPC_ASSUME_ALIGNED
#define RBIPC_ASSUME_ALIGNED(p, a) __builtin_assume_aligned((p), (a))
#endif

/**
 * @def RBIPC_UNREACHABLE()
 * @brief Informs the optimizer that execution flow cannot reach the annotated location.
 * @details Exploits undefined behavior to prune dead code paths and facilitate value-range propagation.
 */
#ifndef RBIPC_UNREACHABLE
#define RBIPC_UNREACHABLE() __builtin_unreachable()
#endif

/**
 * @def RBIPC_ASSUME(cond)
 * @brief Informs the optimizer that Boolean invariant @p cond is universally guaranteed.
 * @details Evaluates @p cond at compile time and emits `__builtin_unreachable()` if false.
 * Enables algebraic simplifications without generating runtime checks.
 */
#ifndef RBIPC_ASSUME
#define RBIPC_ASSUME(cond) do { if (!(cond)) __builtin_unreachable(); } while (0)
#endif

/**
 * @def RBIPC_RESTRICT
 * @brief ISO C23 restrict pointer qualifier asserting non-aliasing memory isolation.
 */
#ifndef RBIPC_RESTRICT
#if defined(__cplusplus)
#define RBIPC_RESTRICT __restrict__
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L
#define RBIPC_RESTRICT restrict
#else
#define RBIPC_RESTRICT __restrict__
#endif
#endif

/**
 * @defgroup rbipc_prefetch Prefetch Parameters
 * @brief Memory access and cache hints for hardware prefetching.
 * @{
 */
#define RBIPC_PREFETCH_READ 0  /**< Hardware prefetch for read access. */
#define RBIPC_PREFETCH_WRITE 1 /**< Hardware prefetch for write access. */
#define RBIPC_PREFETCH_LOCALITY_NONE 0 /**< No temporal cache locality. */
#define RBIPC_PREFETCH_LOCALITY_LOW 1  /**< Low temporal cache locality. */
#define RBIPC_PREFETCH_LOCALITY_MED 2  /**< Moderate temporal cache locality. */
#define RBIPC_PREFETCH_LOCALITY_HIGH 3 /**< High temporal cache locality (retain in L1 cache). */
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
#define RBIPC_INTERNAL
#define RBIPC_INLINE static inline
#define RBIPC_NOINLINE
#define RBIPC_FLATTEN
#define RBIPC_NODISCARD
#define RBIPC_RETURNS_NONNULL
#define RBIPC_PURE
#define RBIPC_CONST
#define RBIPC_HOT
#define RBIPC_COLD
#define RBIPC_LEAF
#define RBIPC_LIKELY(x) (x)
#define RBIPC_UNLIKELY(x) (x)
#define RBIPC_ASSUME_ALIGNED(p, a) (p)
#define RBIPC_UNREACHABLE() ((void)0)
#define RBIPC_ASSUME(cond) ((void)0)
#define RBIPC_RESTRICT
#define RBIPC_PREFETCH_READ 0
#define RBIPC_PREFETCH_WRITE 1
#define RBIPC_PREFETCH_LOCALITY_NONE 0
#define RBIPC_PREFETCH_LOCALITY_LOW 1
#define RBIPC_PREFETCH_LOCALITY_MED 2
#define RBIPC_PREFETCH_LOCALITY_HIGH 3
#define RBIPC_PREFETCH(addr, rw, loc) ((void)0)
#endif

#endif /* RBIPC_ATTR_H */
