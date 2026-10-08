/**
 * @file rbipc_futex.h
 * @brief Low-level Linux futex syscall interfaces
 */

#ifndef RBIPC_FUTEX_H
#define RBIPC_FUTEX_H

#include "rbipc.h"
#include "rbipc_attr.h"

#include <stdint.h>
#include <stdatomic.h>
#include <time.h>

/**
 * @brief Convert nanoseconds to a struct timespec.
 *
 * @param ns Duration in nanoseconds.
 * @param[out] ts Output timespec structure.
 */
RBIPC_LEAF
void rbipc_ns_to_timespec(uint64_t ns, struct timespec * RBIPC_RESTRICT ts);

/**
 * @brief Wait on a process-shared futex word until value changes or timeout expires.
 *
 * @param uaddr Pointer to atomic futex word in shared memory.
 * @param val Expected value at uaddr.
 * @param timeout Optional timeout specification (NULL for infinite).
 * @return 0 on success/wake, negative error code on failure (e.g. -ETIMEDOUT, -EWOULDBLOCK).
 */
RBIPC_LEAF
int rbipc_futex_wait(_Atomic uint32_t *uaddr, uint32_t val, const struct timespec * RBIPC_RESTRICT timeout);

/**
 * @brief Wake up to 'count' waiters on the futex word.
 *
 * @param uaddr Pointer to atomic futex word in shared memory.
 * @param count Number of waiting threads/processes to awaken.
 * @return Number of woken processes, or negative errno on error.
 */
RBIPC_LEAF
int rbipc_futex_wake(_Atomic uint32_t *uaddr, int count);

#endif /* RBIPC_FUTEX_H */
