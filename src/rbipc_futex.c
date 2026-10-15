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

RBIPC_LEAF
bool rbipc_futex_has_waitv(void) {
#if defined(__linux__) && defined(SYS_futex_waitv)
  static _Atomic int s_has_waitv = -1;
  int cached = atomic_load_explicit(&s_has_waitv, memory_order_acquire);
  if (RBIPC_LIKELY(cached != -1)) {
    return cached == 1;
  }
  long ret = syscall(SYS_futex_waitv, NULL, 0, 0, NULL, 0);
  int res = (ret < 0 && errno == ENOSYS) ? 0 : 1;
  atomic_store_explicit(&s_has_waitv, res, memory_order_release);
  return res == 1;
#else
  return false;
#endif
}

static bool rbipc_futex2_has_u64(void) {
#if defined(__linux__) && defined(SYS_futex_waitv)
  static _Atomic int s_has_u64 = -1;
  int cached = atomic_load_explicit(&s_has_u64, memory_order_acquire);
  if (RBIPC_LIKELY(cached != -1)) {
    return cached == 1;
  }
  if (!rbipc_futex_has_waitv()) {
    atomic_store_explicit(&s_has_u64, 0, memory_order_release);
    return false;
  }
  uint64_t dummy = 0;
  struct futex_waitv w = {
      .val = 1,
      .uaddr = (uintptr_t)&dummy,
      .flags = FUTEX2_SIZE_U64,
      .__reserved = 0,
  };
  long ret = syscall(SYS_futex_waitv, &w, 1, 0, NULL, CLOCK_MONOTONIC);
  int res = (ret < 0 && errno == EINVAL) ? 0 : 1;
  atomic_store_explicit(&s_has_u64, res, memory_order_release);
  return res == 1;
#else
  return false;
#endif
}

RBIPC_LEAF
int rbipc_futex_waitv(const struct futex_waitv *waiters, unsigned int nr_waiters,
                      uint64_t timeout_ns) {
#if defined(__linux__) && defined(SYS_futex_waitv)
  if (RBIPC_UNLIKELY(rbipc_is_null(waiters) || nr_waiters == 0 ||
                     nr_waiters > FUTEX_WAITV_MAX)) {
    return -EINVAL;
  }

  struct timespec ts;
  struct timespec *pts = NULL;
  if (timeout_ns != UINT64_MAX) {
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t sec_add = timeout_ns / RBIPC_NSEC_PER_SEC;
    uint64_t nsec_add = timeout_ns % RBIPC_NSEC_PER_SEC;
    ts.tv_sec += (time_t)sec_add;
    ts.tv_nsec += (long)nsec_add;
    if (ts.tv_nsec >= (long)RBIPC_NSEC_PER_SEC) {
      ts.tv_sec += 1;
      ts.tv_nsec -= (long)RBIPC_NSEC_PER_SEC;
    }
    pts = &ts;
  }

  long ret = syscall(SYS_futex_waitv, waiters, nr_waiters, 0, pts, CLOCK_MONOTONIC);
  if (ret < 0) {
    return -errno;
  }
  return (int)ret;
#else
  (void)waiters;
  (void)nr_waiters;
  (void)timeout_ns;
  return -ENOSYS;
#endif
}

/**
 * @brief Normalizes futex wait status codes to standard rbipc status codes.
 */
RBIPC_INLINE int rbipc_futex_normalize_wait_status(int rc) {
  if (rc >= 0 || rc == -EAGAIN || rc == -EWOULDBLOCK) {
    return RBIPC_OK;
  }
  if (rc == -ETIMEDOUT) {
    return RBIPC_ERR_TIMEOUT;
  }
  return rc;
}

RBIPC_LEAF
int rbipc_futex_wait_or_shutdown(_Atomic uint32_t *uaddr, uint32_t val,
                                 const _Atomic uint32_t *shutdown_flag,
                                 uint64_t timeout_ns) {
  if (RBIPC_UNLIKELY(rbipc_is_null(uaddr))) {
    return RBIPC_ERR_INVAL;
  }

  if (shutdown_flag && rbipc_futex_has_waitv()) {
    struct futex_waitv waiters[2];
    waiters[0].val = val;
    waiters[0].uaddr = (uintptr_t)uaddr;
    waiters[0].flags = FUTEX2_SIZE_U32;
    waiters[0].__reserved = 0;

    waiters[1].val = 0;
    waiters[1].uaddr = (uintptr_t)shutdown_flag;
    waiters[1].flags = FUTEX2_SIZE_U32;
    waiters[1].__reserved = 0;

    int rc = rbipc_futex_waitv(waiters, 2, timeout_ns);
    int status = rbipc_futex_normalize_wait_status(rc);
    if (rc != -ENOSYS) {
      return status;
    }
  }

  struct timespec ts;
  struct timespec *pts = NULL;
  if (timeout_ns != UINT64_MAX) {
    rbipc_ns_to_timespec(timeout_ns, &ts);
    pts = &ts;
  }
  int rc = rbipc_futex_wait(uaddr, val, pts);
  return rbipc_futex_normalize_wait_status(rc);
}

RBIPC_LEAF
int rbipc_futex2_wait_slot_or_proxy_or_shutdown(
    const _Atomic uint64_t *slot_seq, uint64_t expected_seq,
    _Atomic uint32_t *proxy_seq, const _Atomic uint32_t *shutdown_flag,
    uint64_t timeout_ns) {
  if (RBIPC_UNLIKELY(rbipc_is_null(slot_seq))) {
    return RBIPC_ERR_INVAL;
  }

  if (rbipc_futex_has_waitv()) {
    struct futex_waitv waiters[3];
    unsigned int nr = 0;
    bool use_u64 = rbipc_futex2_has_u64();

    waiters[nr].val = use_u64 ? expected_seq : (uint32_t)expected_seq;
    waiters[nr].uaddr = (uintptr_t)slot_seq;
    waiters[nr].flags = use_u64 ? FUTEX2_SIZE_U64 : FUTEX2_SIZE_U32;
    waiters[nr].__reserved = 0;
    nr++;

    if (proxy_seq) {
      waiters[nr].val = atomic_load_explicit(proxy_seq, memory_order_relaxed);
      waiters[nr].uaddr = (uintptr_t)proxy_seq;
      waiters[nr].flags = FUTEX2_SIZE_U32;
      waiters[nr].__reserved = 0;
      nr++;
    }

    if (shutdown_flag) {
      waiters[nr].val = 0;
      waiters[nr].uaddr = (uintptr_t)shutdown_flag;
      waiters[nr].flags = FUTEX2_SIZE_U32;
      waiters[nr].__reserved = 0;
      nr++;
    }

    int rc = rbipc_futex_waitv(waiters, nr, timeout_ns);
    int status = rbipc_futex_normalize_wait_status(rc);
    if (rc != -ENOSYS) {
      return status;
    }
  }

  /* Fallback: wait on proxy_seq if available, else slot_seq */
  if (proxy_seq) {
    uint32_t cur_val = atomic_load_explicit(proxy_seq, memory_order_relaxed);
    return rbipc_futex_wait_or_shutdown(proxy_seq, cur_val, shutdown_flag,
                                        timeout_ns);
  }

  struct timespec ts;
  struct timespec *pts = NULL;
  if (timeout_ns != UINT64_MAX) {
    rbipc_ns_to_timespec(timeout_ns, &ts);
    pts = &ts;
  }
  _Atomic uint32_t *low_uaddr = (_Atomic uint32_t *)(void *)slot_seq;
  int rc = rbipc_futex_wait(low_uaddr, (uint32_t)expected_seq, pts);
  return rbipc_futex_normalize_wait_status(rc);
}

RBIPC_LEAF
int rbipc_futex2_wait_slot_or_shutdown(const _Atomic uint64_t *slot_seq,
                                       uint64_t expected_seq,
                                       const _Atomic uint32_t *shutdown_flag,
                                       uint64_t timeout_ns) {
  return rbipc_futex2_wait_slot_or_proxy_or_shutdown(
      slot_seq, expected_seq, NULL, shutdown_flag, timeout_ns);
}

RBIPC_LEAF
int rbipc_futex_wake_op(_Atomic uint32_t *uaddr1, int val1,
                        _Atomic uint32_t *uaddr2, int val2,
                        uint32_t op, uint32_t oparg,
                        uint32_t cmp, uint32_t cmparg) {
#if defined(__linux__) && defined(FUTEX_WAKE_OP)
  if (RBIPC_UNLIKELY(rbipc_is_null(uaddr1) || rbipc_is_null(uaddr2))) {
    return -EINVAL;
  }
  uint32_t val3 = (((op & 0x0fU) << 28U) | ((cmp & 0x0fU) << 24U) |
                   ((oparg & 0x0fffU) << 12U) | (cmparg & 0x0fffU));
  long ret = syscall(SYS_futex, (uint32_t *)uaddr1, FUTEX_WAKE_OP, val1, val2,
                     (uint32_t *)uaddr2, val3);
  if (ret < 0) {
    return -errno;
  }
  return (int)ret;
#else
  (void)uaddr1; (void)val1; (void)uaddr2; (void)val2;
  (void)op; (void)oparg; (void)cmp; (void)cmparg;
  return -ENOSYS;
#endif
}
