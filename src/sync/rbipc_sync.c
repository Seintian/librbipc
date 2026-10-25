/**
 * @file rbipc_sync.c
 * @brief Implementation of the zero-spin passive futex synchronization engine.
 *
 * @details Manages thread-local monotonic deadlines, waiter registration with
 * sequential consistency, and futex suspension and wake dispatches.
 *
 * @author Seintian
 * @date 2026
 * @copyright Apache License 2.0
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "rbipc_sync.h"
#include "rbipc.h"
#include "rbipc_arch.h"
#include "rbipc_attr.h"
#include "rbipc_futex.h"
#include "rbipc_predicate.h"

#include <errno.h>
#include <stdio.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/membarrier.h>
#include <sys/syscall.h>

#ifndef SYS_membarrier
#if defined(__NR_membarrier)
#define SYS_membarrier __NR_membarrier
#elif defined(__mips__)
#define SYS_membarrier 4358
#elif defined(__alpha__)
#define SYS_membarrier 518
#elif defined(__x86_64__)
#define SYS_membarrier 324
#elif defined(__aarch64__) || defined(__riscv)
#define SYS_membarrier 283
#elif defined(__i386__) || defined(__powerpc__)
#define SYS_membarrier 375
#elif defined(__arm__) || defined(__s390x__)
#define SYS_membarrier 389
#endif
#endif
#endif

/**
 * @brief Initialize synchronization state and deadline timestamp.
 */
RBIPC_LEAF
void rbipc_sync_state_init(rbipc_sync_state_t *RBIPC_RESTRICT state,
                           uint64_t timeout_ns) {
  if (RBIPC_UNLIKELY(!state))
    return;
  state->hole_first_seen_ns = 0;
  if (RBIPC_LIKELY(timeout_ns == UINT64_MAX)) {
    state->has_deadline = false;
    state->deadline_ns = 0;
  } else if (RBIPC_UNLIKELY(timeout_ns == 0)) {
    state->has_deadline = true;
    state->deadline_ns = 0;
  } else {
    state->has_deadline = true;
    state->deadline_ns = rbipc_clock_monotonic_ns() + timeout_ns;
  }
}

static _Atomic int s_membarrier_cmd = -1;

/**
 * @brief Initialize process-level synchronization capabilities (e.g. sys_membarrier registration).
 */
RBIPC_LEAF
void rbipc_sync_init_process(void) {
#if defined(__linux__) && defined(SYS_membarrier)
  int cmd = atomic_load_explicit(&s_membarrier_cmd, memory_order_acquire);
  if (RBIPC_UNLIKELY(cmd == -1)) {
    int new_cmd = 0;
    long sup_ret = syscall(SYS_membarrier, MEMBARRIER_CMD_QUERY, 0, 0);
    if (sup_ret > 0) {
      uint32_t sup = (uint32_t)sup_ret;
      if ((sup & (uint32_t)MEMBARRIER_CMD_GLOBAL_EXPEDITED) != 0U) {
        if (syscall(SYS_membarrier, MEMBARRIER_CMD_REGISTER_GLOBAL_EXPEDITED, 0,
                    0) == 0) {
          new_cmd = MEMBARRIER_CMD_GLOBAL_EXPEDITED;
        }
      }
      if (new_cmd == 0) {
        if ((sup & (uint32_t)MEMBARRIER_CMD_GLOBAL) != 0U) {
          new_cmd = MEMBARRIER_CMD_GLOBAL;
        }
      }
    }
    atomic_store_explicit(&s_membarrier_cmd, new_cmd, memory_order_release);
  }
#endif
}

/**
 * @brief Execute an asymmetric memory barrier on the slow wait path (Linux sys_membarrier).
 */
RBIPC_LEAF
void rbipc_sync_asymmetric_barrier(void) {
#if defined(__linux__) && defined(SYS_membarrier)
  int cmd = atomic_load_explicit(&s_membarrier_cmd, memory_order_acquire);
  if (RBIPC_UNLIKELY(cmd == -1)) {
    rbipc_sync_init_process();
    cmd = atomic_load_explicit(&s_membarrier_cmd, memory_order_acquire);
  }

  if (RBIPC_LIKELY(cmd > 0)) {
    if (syscall(SYS_membarrier, cmd, 0, 0) == 0) {
      return;
    }
  }
#endif
  atomic_thread_fence(memory_order_seq_cst);
}

/**
 * @brief Register intent to wait by incrementing waiter counter and issuing asymmetric barrier.
 */
RBIPC_LEAF
void rbipc_sync_register_waiter(_Atomic uint32_t *futex_waiters) {
  if (futex_waiters) {
    atomic_fetch_add_explicit(futex_waiters, 1, memory_order_seq_cst);
    rbipc_sync_asymmetric_barrier();
  }
}

/**
 * @brief Deregister wait intent by decrementing waiter counter.
 */
RBIPC_LEAF
void rbipc_sync_deregister_waiter(_Atomic uint32_t *futex_waiters) {
  if (futex_waiters) {
    atomic_fetch_sub_explicit(futex_waiters, 1, memory_order_seq_cst);
  }
}

/**
 * @brief Execute futex wait when waiter is already registered, checking cooperative shutdown.
 */
RBIPC_LEAF
int rbipc_sync_wait_registered_or_shutdown(
    rbipc_sync_state_t *RBIPC_RESTRICT state, _Atomic uint32_t *futex_word,
    _Atomic uint32_t *futex_waiters, const _Atomic uint32_t *shutdown_flag) {
  if (RBIPC_UNLIKELY(!state || !futex_word)) {
    rbipc_sync_deregister_waiter(futex_waiters);
    return RBIPC_ERR_INVAL;
  }

  uint64_t now = rbipc_clock_monotonic_ns();
  if (RBIPC_UNLIKELY(rbipc_sync_is_expired(state, now))) {
    rbipc_sync_deregister_waiter(futex_waiters);
    return RBIPC_ERR_TIMEOUT;
  }

  uint64_t remaining_ns = RBIPC_FUTEX_PERIOD_NS;
  if (state->has_deadline) {
    if (RBIPC_UNLIKELY(now >= state->deadline_ns)) {
      rbipc_sync_deregister_waiter(futex_waiters);
      return RBIPC_ERR_TIMEOUT;
    }
    remaining_ns = state->deadline_ns - now;
    if (remaining_ns > RBIPC_FUTEX_PERIOD_NS) {
      remaining_ns = RBIPC_FUTEX_PERIOD_NS;
    }
  }

  uint32_t current_val = atomic_load_explicit(futex_word, memory_order_relaxed);
  int rc = rbipc_futex_wait_or_shutdown(futex_word, current_val, shutdown_flag,
                                        remaining_ns);
  rbipc_sync_deregister_waiter(futex_waiters);

  if (rc == RBIPC_ERR_TIMEOUT && state->has_deadline) {
    uint64_t post_now = rbipc_clock_monotonic_ns();
    if (post_now >= state->deadline_ns) {
      return RBIPC_ERR_TIMEOUT;
    }
  }
  return 0;
}

RBIPC_LEAF
int rbipc_sync_wait_slot_registered(
    rbipc_sync_state_t *RBIPC_RESTRICT state, const _Atomic uint64_t *slot_seq,
    uint64_t expected_seq, _Atomic uint32_t *proxy_seq,
    _Atomic uint32_t *futex_waiters, const _Atomic uint32_t *shutdown_flag) {
  if (RBIPC_UNLIKELY(!state || !slot_seq)) {
    rbipc_sync_deregister_waiter(futex_waiters);
    return RBIPC_ERR_INVAL;
  }

  uint64_t now = rbipc_clock_monotonic_ns();
  if (RBIPC_UNLIKELY(rbipc_sync_is_expired(state, now))) {
    rbipc_sync_deregister_waiter(futex_waiters);
    return RBIPC_ERR_TIMEOUT;
  }

  uint64_t remaining_ns = RBIPC_FUTEX_PERIOD_NS;
  if (state->has_deadline) {
    if (RBIPC_UNLIKELY(now >= state->deadline_ns)) {
      rbipc_sync_deregister_waiter(futex_waiters);
      return RBIPC_ERR_TIMEOUT;
    }
    remaining_ns = state->deadline_ns - now;
    if (remaining_ns > RBIPC_FUTEX_PERIOD_NS) {
      remaining_ns = RBIPC_FUTEX_PERIOD_NS;
    }
  }

  int rc = rbipc_futex2_wait_slot_or_proxy_or_shutdown(
      slot_seq, expected_seq, proxy_seq, shutdown_flag, remaining_ns);
  rbipc_sync_deregister_waiter(futex_waiters);

  if (rc == RBIPC_ERR_TIMEOUT && state->has_deadline) {
    uint64_t post_now = rbipc_clock_monotonic_ns();
    if (post_now >= state->deadline_ns) {
      return RBIPC_ERR_TIMEOUT;
    }
  }
  return (rc == RBIPC_ERR_TIMEOUT) ? 0 : rc;
}

/**
 * @brief Execute futex wait when waiter is already registered.
 */
RBIPC_LEAF
int rbipc_sync_wait_registered(rbipc_sync_state_t *RBIPC_RESTRICT state,
                               _Atomic uint32_t *futex_word,
                               _Atomic uint32_t *futex_waiters) {
  return rbipc_sync_wait_registered_or_shutdown(state, futex_word,
                                                futex_waiters, NULL);
}

/**
 * @brief Passive futex backoff step with automatic waiter registration.
 */
RBIPC_LEAF
int rbipc_sync_backoff(rbipc_sync_state_t *RBIPC_RESTRICT state,
                       _Atomic uint32_t *futex_word,
                       _Atomic uint32_t *futex_waiters) {
  if (RBIPC_UNLIKELY(!state || !futex_word)) {
    return RBIPC_ERR_INVAL;
  }
  rbipc_sync_register_waiter(futex_waiters);
  return rbipc_sync_wait_registered(state, futex_word, futex_waiters);
}

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) ||             \
    defined(_M_IX86)
static uint64_t rbipc_get_tsc_ticks_per_ns(void) {
  const rbipc_hardware_profile_t *prof = rbipc_get_hardware_profile();
  return (prof && prof->tsc_ticks_per_ns > 0) ? prof->tsc_ticks_per_ns : 1;
}
#endif

/**
 * @brief Execute a hardware-assisted monitor-wait with explicit CPU power substate.
 */
RBIPC_LEAF int
rbipc_sync_hardware_wait_substate(const void *monitored_addr, uint64_t timeout_ns,
                                  rbipc_spin_pred_t predicate, void *arg,
                                  uint32_t substate) {
  if (RBIPC_UNLIKELY(!monitored_addr)) {
    return RBIPC_ERR_INVAL;
  }
  if (predicate && predicate(arg)) {
    return RBIPC_OK;
  }
  if (timeout_ns == 0) {
    return RBIPC_ERR_TIMEOUT;
  }
  if (timeout_ns > RBIPC_HW_WAIT_TIMEOUT_NS) {
    timeout_ns = RBIPC_HW_WAIT_TIMEOUT_NS;
  }

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) ||             \
    defined(_M_IX86)
  if (rbipc_cpu_has_waitpkg()) {
    rbipc_cpu_umonitor(monitored_addr);
    if (predicate && predicate(arg)) {
      return RBIPC_OK;
    }
    uint64_t ticks_per_ns = rbipc_get_tsc_ticks_per_ns();
    uint64_t tsc_deadline = rbipc_rdtsc() + (timeout_ns * ticks_per_ns);
    /* Substate: 0 = C0.2 deep power save, 1 = C0.1 fast wake */
    rbipc_cpu_umwait(substate != 0 ? 1 : 0, tsc_deadline);
    if (predicate && predicate(arg)) {
      return RBIPC_OK;
    }
    return RBIPC_ERR_TIMEOUT;
  }
#elif defined(__aarch64__)
  if (rbipc_cpu_has_wfxt()) {
    uint64_t timer_freq = rbipc_arm_cntfrq();
    uint64_t cur_cnt = rbipc_arm_cntvct();
    uint64_t target_cnt = cur_cnt + (timeout_ns * timer_freq / 1000000000ULL);

    uint64_t dummy = 0;
    __asm__ __volatile__("ldaxr %0, [%1]"
                         : "=r"(dummy)
                         : "r"(monitored_addr)
                         : "memory");
    if (predicate && predicate(arg)) {
      __asm__ __volatile__("clrex" ::: "memory");
      return RBIPC_OK;
    }
    __asm__ __volatile__("wfet %0" :: "r"(target_cnt) : "memory");
    __asm__ __volatile__("clrex" ::: "memory");
    if (predicate && predicate(arg)) {
      return RBIPC_OK;
    }
    return RBIPC_ERR_TIMEOUT;
  }
#else
  (void)monitored_addr;
  (void)timeout_ns;
  (void)predicate;
  (void)arg;
  (void)substate;
#endif

  return -ENOSYS;
}

/**
 * @brief Execute a hardware-assisted monitor-wait (Tier 1: Intel WAITPKG / ARM64 WFET).
 */
RBIPC_LEAF int
rbipc_sync_hardware_wait(const void *monitored_addr, uint64_t timeout_ns,
                         rbipc_spin_pred_t predicate, void *arg) {
  return rbipc_sync_hardware_wait_substate(monitored_addr, timeout_ns,
                                           predicate, arg, 1);
}

/**
 * @brief Adaptive two-phase spin-then-wait backoff engine.
 */
RBIPC_LEAF
int rbipc_sync_backoff_adaptive(rbipc_sync_state_t *RBIPC_RESTRICT state,
                                _Atomic uint32_t *futex_word,
                                _Atomic uint32_t *futex_waiters,
                                uint32_t policy, uint32_t spin_budget,
                                rbipc_spin_pred_t predicate, void *arg) {
  if (RBIPC_UNLIKELY(!state || !futex_word)) {
    return RBIPC_ERR_INVAL;
  }
  if ((policy & RBIPC_WAIT_ADAPTIVE) && predicate) {
    uint32_t budget =
        (spin_budget > 0) ? spin_budget : rbipc_sync_spin_count();
    for (uint32_t i = 0; i < budget; ++i) {
      rbipc_cpu_pause();
      if (predicate(arg)) {
        return 0;
      }
    }
  }
  if (((policy & RBIPC_WAIT_ADAPTIVE) || (policy & RBIPC_WAIT_HW_MONITOR)) &&
      predicate) {
    if (rbipc_cpu_has_waitpkg() || rbipc_cpu_has_wfxt()) {
      uint32_t substate = (policy & RBIPC_WAIT_ADAPTIVE) ? 1 : 0;
      int hw_rc = rbipc_sync_hardware_wait_substate(futex_word, RBIPC_HW_WAIT_TIMEOUT_NS,
                                                    predicate, arg, substate);
      if (hw_rc == 0) {
        return 0;
      }
    }
  }
  return rbipc_sync_backoff(state, futex_word, futex_waiters);
}

/**
 * @brief Advance futex sequence and notify sleeping waiters if present.
 *
 * @param[in,out] futex_word    Atomic sequence counter in shared memory.
 * @param[in]     futex_waiters Atomic waiter count.
 * @param[in]     count         Number of tasks to awaken.
 */
RBIPC_INLINE void rbipc_sync_dispatch_wake(_Atomic uint32_t *futex_word,
                                           _Atomic uint32_t *futex_waiters,
                                           int count) {
  if (RBIPC_UNLIKELY(!futex_word))
    return;

  /*
   * Increment monotonic sequence counter with release semantics.
   * Avoid kernel FUTEX_WAKE_OP comparison signed-overflow hazards (FUTEX_OP_CMP_GE)
   * which stall thread wakeups permanently once counter reaches 2^31 (signed negative).
   * Direct user-space atomic increment combined with FUTEX_WAKE guarantees reliable,
   * overflow-immune thread resumption across all 2^32 counter values.
   */
  atomic_fetch_add_explicit(futex_word, 1, memory_order_release);
  if (rbipc_sync_should_wake(futex_waiters)) {
    rbipc_futex_wake(futex_word, count);
  }
}

/**
 * @brief Wake up to count waiting threads via sys_futex.
 */
RBIPC_LEAF
void rbipc_sync_wake_n(_Atomic uint32_t *futex_word,
                       _Atomic uint32_t *futex_waiters, uint32_t count) {
  int c = (count > (uint32_t)INT32_MAX) ? INT32_MAX : (int)count;
  if (c <= 0)
    c = 1;
  rbipc_sync_dispatch_wake(futex_word, futex_waiters, c);
}

/**
 * @brief Wake one waiting thread via sys_futex.
 */
RBIPC_LEAF
void rbipc_sync_wake_one(_Atomic uint32_t *futex_word,
                         _Atomic uint32_t *futex_waiters) {
  rbipc_sync_dispatch_wake(futex_word, futex_waiters, 1);
}

/**
 * @brief Wake all waiting threads via sys_futex broadcast.
 */
RBIPC_LEAF
void rbipc_sync_wake_all(_Atomic uint32_t *futex_word,
                         _Atomic uint32_t *futex_waiters) {
  rbipc_sync_dispatch_wake(futex_word, futex_waiters, INT32_MAX);
}
