/**
 * @file rbipc_attr.h
 * @brief Internal compiler annotations, attributes, and micro-optimization macros
 */

#ifndef RBIPC_ATTR_H
#define RBIPC_ATTR_H

#include "rbipc.h"

#if defined(__GNUC__) || defined(__clang__)
#define RBIPC_INLINE            static inline __attribute__((always_inline))
#define RBIPC_NOINLINE          __attribute__((noinline))
#define RBIPC_UNUSED            __attribute__((unused))
#define RBIPC_FLATTEN           __attribute__((flatten))
#define RBIPC_WARN_UNUSED       __attribute__((warn_unused_result))
#define RBIPC_INTERNAL_NONNULL(...) __attribute__((nonnull(__VA_ARGS__)))
#define RBIPC_PREFETCH(addr, rw, loc) __builtin_prefetch((addr), (rw), (loc))
#define RBIPC_ASSUME(cond)      do { if (!(cond)) RBIPC_UNREACHABLE(); } while (0)
#else
#define RBIPC_INLINE            static inline
#define RBIPC_NOINLINE
#define RBIPC_UNUSED
#define RBIPC_FLATTEN
#define RBIPC_WARN_UNUSED
#define RBIPC_INTERNAL_NONNULL(...)
#define RBIPC_PREFETCH(addr, rw, loc) ((void)0)
#define RBIPC_ASSUME(cond)      ((void)0)
#endif

#endif /* RBIPC_ATTR_H */

