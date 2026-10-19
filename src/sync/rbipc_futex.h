/**
 * @file rbipc_futex.h
 * @brief Low-level Linux Fast Userspace Mutex (sys_futex) kernel syscall
 * interfaces.
 *
 * @details Provides process-shared futex synchronization primitives for
 * suspending and waking threads across memory-mapped files without spinning or
 * burning CPU cycles.
 *
 * @author Seintian
 * @date 2026
 * @copyright Apache License 2.0
 */

#ifndef RBIPC_FUTEX_H
#define RBIPC_FUTEX_H

#include "rbipc.h"
#include "rbipc_attr.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#if defined(__linux__)
#include <linux/futex.h>
#endif

#ifndef SYS_futex_waitv
#if defined(__x86_64__) || defined(__aarch64__) || defined(__i386__) ||        \
    defined(__arm__) || defined(__riscv)
#define SYS_futex_waitv 449
#endif
#endif

#ifndef FUTEX2_SIZE_U8
#define FUTEX2_SIZE_U8 0x00
#endif
#ifndef FUTEX2_SIZE_U16
#define FUTEX2_SIZE_U16 0x01
#endif
#ifndef FUTEX2_SIZE_U32
#define FUTEX2_SIZE_U32 0x02
#endif
#ifndef FUTEX2_SIZE_U64
#define FUTEX2_SIZE_U64 0x03
#endif
#ifndef FUTEX2_PRIVATE
#define FUTEX2_PRIVATE 128
#endif
#ifndef FUTEX_WAITV_MAX
#define FUTEX_WAITV_MAX 128
#endif

#ifndef FUTEX_WAKE_OP
#define FUTEX_WAKE_OP 5
#endif
#ifndef FUTEX_OP_SET
#define FUTEX_OP_SET 0
#endif
#ifndef FUTEX_OP_ADD
#define FUTEX_OP_ADD 1
#endif
#ifndef FUTEX_OP_OR
#define FUTEX_OP_OR 2
#endif
#ifndef FUTEX_OP_ANDN
#define FUTEX_OP_ANDN 3
#endif
#ifndef FUTEX_OP_XOR
#define FUTEX_OP_XOR 4
#endif

#ifndef FUTEX_OP_CMP_EQ
#define FUTEX_OP_CMP_EQ 0
#endif
#ifndef FUTEX_OP_CMP_NE
#define FUTEX_OP_CMP_NE 1
#endif
#ifndef FUTEX_OP_CMP_LT
#define FUTEX_OP_CMP_LT 2
#endif
#ifndef FUTEX_OP_CMP_LE
#define FUTEX_OP_CMP_LE 3
#endif
#ifndef FUTEX_OP_CMP_GT
#define FUTEX_OP_CMP_GT 4
#endif
#ifndef FUTEX_OP_CMP_GE
#define FUTEX_OP_CMP_GE 5
#endif

#if !defined(__linux__) || !defined(FUTEX_32)
/**
 * @struct futex_waitv
 * @brief Descriptor for vectorized futex wait operations (Linux 5.16+).
 */
struct futex_waitv {
  uint64_t val;        /**< Expected value at uaddr. */
  uint64_t uaddr;      /**< User-space pointer to wait on. */
  uint32_t flags;      /**< Flags: FUTEX2_SIZE_U32, etc. */
  uint32_t __reserved; /**< Reserved, must be 0. */
};
#endif

/**
 * @brief Decompose a 64-bit nanosecond duration into a POSIX struct timespec.
 *
 * @param[in]  ns Duration in nanoseconds.
 * @param[out] ts Pointer to caller-allocated struct timespec to populate.
 */
RBIPC_INTERNAL RBIPC_LEAF
void rbipc_ns_to_timespec(uint64_t ns, struct timespec *RBIPC_RESTRICT ts);

/**
 * @brief Suspend the calling thread on a process-shared futex word until
 * modified or timed out.
 *
 * @details Dispatches `SYS_futex` with `FUTEX_WAIT`. The Linux kernel checks
 * whether `*uaddr == val` atomically before descheduling the calling task. If
 * the value has already changed, the syscall returns immediately with
 * `-EWOULDBLOCK` without sleeping.
 *
 * @param[in] uaddr   Direct pointer to 32-bit atomic futex integer in shared
 * memory.
 * @param[in] val     Expected value at @p uaddr. If `*uaddr != val`, syscall
 * returns immediately.
 * @param[in] timeout Pointer to relative timeout timespec, or NULL for
 * unbounded suspension.
 *
 * @return 0 on successful wake notification.
 * @retval -EWOULDBLOCK if `*uaddr != val` when entering kernel.
 * @retval -ETIMEDOUT   if @p timeout expired before wake notification.
 * @retval -EINTR       if interrupted by a POSIX signal.
 * @retval -EINVAL      if @p uaddr is null or misaligned.
 */
RBIPC_INTERNAL RBIPC_LEAF
int rbipc_futex_wait(_Atomic uint32_t *uaddr, uint32_t val,
                     const struct timespec *RBIPC_RESTRICT timeout);

/**
 * @brief Awaken waiting threads or processes currently suspended on the given
 * futex word.
 *
 * @details Dispatches `SYS_futex` with `FUTEX_WAKE` to transition up to @p
 * count waiters from the kernel wait-queue to the runnable state.
 *
 * @param[in] uaddr Direct pointer to 32-bit atomic futex integer in shared
 * memory.
 * @param[in] count Maximum number of waiters to awaken (e.g. 1 for
 * single-consumer, INT_MAX for broadcast).
 *
 * @return Non-negative count of successfully awakened waiters on success.
 * @retval -EINVAL if @p uaddr is null or misaligned.
 * @retval -errno  if underlying futex syscall returns failure.
 */
RBIPC_INTERNAL RBIPC_LEAF
int rbipc_futex_wake(_Atomic uint32_t *uaddr, int count);

/**
 * @brief Awaken waiting threads currently suspended on a 64-bit slot sequence word.
 *
 * @details Dispatches  with  targeting the appropriate 32-bit word
 * boundary of the 64-bit slot sequence based on target machine endianness.
 *
 * @param[in] slot_seq Direct pointer to 64-bit atomic sequence word in shared memory.
 * @param[in] count    Maximum number of waiters to awaken.
 *
 * @return Non-negative count of successfully awakened waiters on success.
 * @retval -EINVAL if @p slot_seq is null.
 * @retval -errno  if underlying futex syscall returns failure.
 */
RBIPC_INTERNAL RBIPC_LEAF
int rbipc_futex_wake_slot(const _Atomic uint64_t *slot_seq, int count);

/**
 * @brief Checks if Linux 5.16+ sys_futex_waitv is supported by the host kernel.
 *
 * @return true if sys_futex_waitv is available, false otherwise.
 */
RBIPC_INTERNAL RBIPC_LEAF
bool rbipc_futex_has_waitv(void);

/**
 * @brief Checks if Linux 5.16+ sys_futex_waitv supports 64-bit futex words (FUTEX2_SIZE_U64).
 *
 * @details Probes kernel capability via non-blocking sys_futex_waitv probe and memoizes
 * the result to eliminate repeated kernel syscall overhead.
 *
 * @return true if FUTEX2_SIZE_U64 is supported by the kernel, false otherwise.
 */
RBIPC_INTERNAL RBIPC_LEAF
bool rbipc_futex2_has_u64(void);

/**
 * @brief Wait on multiple futex words via Linux 5.16+ sys_futex_waitv.
 *
 * @param[in] waiters    Array of futex_waitv descriptors.
 * @param[in] nr_waiters Number of futex entries to monitor (1 to 128).
 * @param[in] timeout_ns Relative timeout duration in nanoseconds (UINT64_MAX for unbounded).
 *
 * @return Non-negative index of the awakened futex entry on success.
 * @retval -EWOULDBLOCK / -EAGAIN if a futex value did not match expected value.
 * @retval -ETIMEDOUT   if timeout duration expired.
 * @retval -ENOSYS      if sys_futex_waitv is unsupported on the host kernel.
 * @retval -EINVAL      if parameters are invalid.
 * @retval -errno       on other system call errors.
 */
RBIPC_INTERNAL RBIPC_LEAF
int rbipc_futex_waitv(const struct futex_waitv *waiters, unsigned int nr_waiters,
                      uint64_t timeout_ns);

/**
 * @brief Wait on a 32-bit primary futex word or cooperative shutdown flag via sys_futex_waitv.
 *
 * @details Atomically monitors both @p uaddr and @p shutdown_flag. If sys_futex_waitv is
 * available, both are registered in the kernel wait-queue; a wake on either or an update to
 * either awakens the caller immediately. If sys_futex_waitv is unsupported, falls back to
 * legacy rbipc_futex_wait on @p uaddr.
 *
 * @param[in] uaddr         Primary 32-bit atomic futex sequence word.
 * @param[in] val           Expected value at @p uaddr.
 * @param[in] shutdown_flag Optional pointer to cooperative shutdown flag (expected 0).
 * @param[in] timeout_ns    Relative timeout duration in nanoseconds (UINT64_MAX for unbounded).
 *
 * @return 0 on successful wake or condition change, RBIPC_ERR_TIMEOUT on expiration, or error.
 */
RBIPC_INTERNAL RBIPC_LEAF
int rbipc_futex_wait_or_shutdown(_Atomic uint32_t *uaddr, uint32_t val,
                                 const _Atomic uint32_t *shutdown_flag,
                                 uint64_t timeout_ns);

/**
 * @brief Wait directly on 64-bit slot sequence or shutdown flag via Linux 5.16+ futex_waitv.
 *
 * @details Probes kernel support for FUTEX2_SIZE_U64. If supported, passes 64-bit slot sequence.
 * If FUTEX2_SIZE_U64 returns -EINVAL (current upstream Linux), monitors the low 32-bit word of
 * slot sequence and the 32-bit shutdown flag using FUTEX2_SIZE_U32.
 * If sys_futex_waitv is unsupported (-ENOSYS), falls back to legacy sys_futex on low 32 bits.
 *
 * @param[in] slot_seq      Pointer to 64-bit atomic sequence word.
 * @param[in] expected_seq  Expected 64-bit sequence value.
 * @param[in] shutdown_flag Pointer to 32-bit cooperative shutdown flag (expected 0).
 * @param[in] timeout_ns    Relative timeout in nanoseconds (UINT64_MAX for unbounded).
 *
 * @return 0 on success / condition satisfied, RBIPC_ERR_TIMEOUT on expiration, or error.
 */
RBIPC_INTERNAL RBIPC_LEAF
int rbipc_futex2_wait_slot_or_shutdown(const _Atomic uint64_t *slot_seq,
                                       uint64_t expected_seq,
                                       const _Atomic uint32_t *shutdown_flag,
                                       uint64_t timeout_ns);

/**
 * @brief Adaptive futex wait: monitors 64-bit slot sequence directly (using FUTEX2_SIZE_U64
 *        or FUTEX2_SIZE_U32), proxy sequence, and shutdown flag via Linux 5.16+ sys_futex_waitv,
 *        or seamlessly falls back to classical 32-bit SYS_futex proxy waiting on older kernels.
 *
 * @param[in] slot_seq      Pointer to 64-bit atomic sequence word.
 * @param[in] expected_seq  Expected 64-bit sequence value.
 * @param[in] proxy_seq     Shared proxy 32-bit sequence word.
 * @param[in] shutdown_flag Pointer to 32-bit cooperative shutdown flag (expected 0).
 * @param[in] timeout_ns    Relative timeout in nanoseconds (UINT64_MAX for unbounded).
 *
 * @return 0 on success / condition satisfied, RBIPC_ERR_TIMEOUT on expiration, or error.
 */
RBIPC_INTERNAL RBIPC_LEAF
int rbipc_futex2_wait_slot_or_proxy_or_shutdown(
    const _Atomic uint64_t *slot_seq, uint64_t expected_seq,
    _Atomic uint32_t *proxy_seq, const _Atomic uint32_t *shutdown_flag,
    uint64_t timeout_ns);

/**
 * @brief Atomically modify a secondary futex word and wake waiters on two distinct
 * futex words in a single kernel syscall via FUTEX_WAKE_OP.
 *
 * @param[in,out] uaddr1 First futex word to unconditionally wake up to @p val1 waiters on.
 * @param[in]     val1   Maximum number of waiters to wake on @p uaddr1.
 * @param[in,out] uaddr2 Second futex word to atomically modify and conditionally wake on.
 * @param[in]     val2   Maximum number of waiters to wake on @p uaddr2 if condition holds.
 * @param[in]     op     Arithmetic operation to apply to @p uaddr2 (e.g. FUTEX_OP_ADD).
 * @param[in]     oparg  Argument for @p op (e.g. 1).
 * @param[in]     cmp    Comparison operator against old value of @p uaddr2 (e.g. FUTEX_OP_CMP_GT).
 * @param[in]     cmparg Value to compare old value against (e.g. 0).
 *
 * @return Total number of awakened waiters on success, or negative errno on error.
 */
RBIPC_INTERNAL RBIPC_LEAF
int rbipc_futex_wake_op(_Atomic uint32_t *uaddr1, int val1,
                        _Atomic uint32_t *uaddr2, int val2,
                        uint32_t op, uint32_t oparg,
                        uint32_t cmp, uint32_t cmparg);

#endif /* RBIPC_FUTEX_H */
