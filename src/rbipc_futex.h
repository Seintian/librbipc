/**
 * @file rbipc_futex.h
 * @brief Low-level Linux Fast Userspace Mutex (sys_futex) kernel syscall interfaces.
 *
 * @details Provides process-shared futex synchronization primitives for suspending and waking
 * threads across memory-mapped files without spinning or burning CPU cycles.
 *
 * @author Christian Santarelli
 * @date 2026
 * @copyright Apache License 2.0
 */

#ifndef RBIPC_FUTEX_H
#define RBIPC_FUTEX_H

#include "rbipc.h"
#include "rbipc_attr.h"

#include <stdint.h>
#include <stdatomic.h>
#include <time.h>

/**
 * @brief Decompose a 64-bit nanosecond duration into a POSIX struct timespec.
 *
 * @param[in]  ns Duration in nanoseconds.
 * @param[out] ts Pointer to caller-allocated struct timespec to populate.
 */
RBIPC_LEAF
void rbipc_ns_to_timespec(uint64_t ns, struct timespec * RBIPC_RESTRICT ts);

/**
 * @brief Suspend the calling thread on a process-shared futex word until modified or timed out.
 *
 * @details Dispatches `SYS_futex` with `FUTEX_WAIT`. The Linux kernel checks whether `*uaddr == val`
 * atomically before descheduling the calling task. If the value has already changed, the syscall
 * returns immediately with `-EWOULDBLOCK` without sleeping.
 *
 * @param[in] uaddr   Direct pointer to 32-bit atomic futex integer in shared memory.
 * @param[in] val     Expected value at @p uaddr. If `*uaddr != val`, syscall returns immediately.
 * @param[in] timeout Pointer to relative timeout timespec, or NULL for unbounded suspension.
 *
 * @return 0 on successful wake notification.
 * @retval -EWOULDBLOCK if `*uaddr != val` when entering kernel.
 * @retval -ETIMEDOUT   if @p timeout expired before wake notification.
 * @retval -EINTR       if interrupted by a POSIX signal.
 * @retval -EINVAL      if @p uaddr is null or misaligned.
 */
RBIPC_LEAF
int rbipc_futex_wait(_Atomic uint32_t *uaddr, uint32_t val, const struct timespec * RBIPC_RESTRICT timeout);

/**
 * @brief Awaken waiting threads or processes currently suspended on the given futex word.
 *
 * @details Dispatches `SYS_futex` with `FUTEX_WAKE` to transition up to @p count waiters from the
 * kernel wait-queue to the runnable state.
 *
 * @param[in] uaddr Direct pointer to 32-bit atomic futex integer in shared memory.
 * @param[in] count Maximum number of waiters to awaken (e.g. 1 for single-consumer, INT_MAX for broadcast).
 *
 * @return Non-negative count of successfully awakened waiters on success.
 * @retval -EINVAL if @p uaddr is null or misaligned.
 * @retval -errno  if underlying futex syscall returns failure.
 */
RBIPC_LEAF
int rbipc_futex_wake(_Atomic uint32_t *uaddr, int count);

#endif /* RBIPC_FUTEX_H */
