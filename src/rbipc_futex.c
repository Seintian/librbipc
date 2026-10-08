/**
 * @file rbipc_futex.c
 * @brief Implementation of Linux sys_futex kernel syscall wrappers.
 * @details Implements timespec duration decomposition, process-shared futex wait suspension,
 *          and waiter wakeup dispatching.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "rbipc.h"
#include "rbipc_futex.h"
#include "rbipc_attr.h"
#include "rbipc_predicate.h"

#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>
#include <linux/futex.h>

/**
 * @brief Splits nanoseconds into seconds and remaining nanoseconds.
 */
RBIPC_LEAF
void rbipc_ns_to_timespec(uint64_t ns, struct timespec * RBIPC_RESTRICT ts) {
    if (RBIPC_UNLIKELY(rbipc_is_null(ts))) {
        return;
    }
    ts->tv_sec = (time_t)(ns / RBIPC_NSEC_PER_SEC);
    ts->tv_nsec = (long)(ns % RBIPC_NSEC_PER_SEC);
}

/**
 * @brief Suspends current thread via FUTEX_WAIT on process-shared memory address.
 */
RBIPC_LEAF
int rbipc_futex_wait(_Atomic uint32_t *uaddr, uint32_t val, const struct timespec * RBIPC_RESTRICT timeout) {
    if (RBIPC_UNLIKELY(rbipc_is_null(uaddr))) {
        return -EINVAL;
    }
    /* Process-shared futex wait (FUTEX_WAIT without FUTEX_PRIVATE_FLAG) */
    long ret = syscall(SYS_futex, (uint32_t *)uaddr, FUTEX_WAIT, val, timeout, NULL, 0);
    if (ret != 0) {
        return -errno;
    }
    return 0;
}

/**
 * @brief Wakes up to @p count waiters on process-shared futex address.
 */
RBIPC_LEAF
int rbipc_futex_wake(_Atomic uint32_t *uaddr, int count) {
    if (RBIPC_UNLIKELY(rbipc_is_null(uaddr))) {
        return -EINVAL;
    }
    long ret = syscall(SYS_futex, (uint32_t *)uaddr, FUTEX_WAKE, count, NULL, NULL, 0);
    if (ret < 0) {
        return -errno;
    }
    return (int)ret;
}
