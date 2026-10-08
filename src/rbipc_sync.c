/**
 * @file rbipc_sync.c
 * @brief Implementation of the zero-spin passive futex synchronization engine.
 *
 * @details Manages thread-local monotonic deadlines, waiter registration with sequential
 * consistency, and futex suspension and wake dispatches.
 *
 * @author Christian Santarelli
 * @date 2026
 * @copyright Apache License 2.0
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "rbipc.h"
#include "rbipc_sync.h"
#include "rbipc_arch.h"
#include "rbipc_futex.h"
#include "rbipc_attr.h"
#include "rbipc_predicate.h"

#include <sched.h>
#include <errno.h>

RBIPC_LEAF
void rbipc_sync_state_init(rbipc_sync_state_t * RBIPC_RESTRICT state, uint64_t timeout_ns) {
    if (RBIPC_UNLIKELY(!state)) return;
    state->spin_count = 0;
    if (timeout_ns == UINT64_MAX) {
        state->has_deadline = false;
        state->deadline_ns = 0;
    } else {
        state->has_deadline = true;
        state->deadline_ns = rbipc_clock_monotonic_ns() + timeout_ns;
    }
}

/**
 * @brief Compute relative timeout timespec capped by RBIPC_FUTEX_PERIOD_NS.
 *
 * @param[in]  state  Sync state with configured deadline.
 * @param[in]  now    Current monotonic timestamp in nanoseconds.
 * @param[out] out_ts Timespec structure receiving the calculated timeout.
 *
 * @return 0 on success.
 * @retval RBIPC_ERR_TIMEOUT if deadline has already expired.
 */
RBIPC_INLINE int rbipc_sync_calc_timeout_spec(const rbipc_sync_state_t * RBIPC_RESTRICT state,
                                                uint64_t now,
                                                struct timespec * RBIPC_RESTRICT out_ts) {
    if (state->has_deadline) {
        if (RBIPC_UNLIKELY(now >= state->deadline_ns)) {
            return RBIPC_ERR_TIMEOUT;
        }
        uint64_t remaining_ns = state->deadline_ns - now;
        if (remaining_ns > RBIPC_FUTEX_PERIOD_NS) {
            remaining_ns = RBIPC_FUTEX_PERIOD_NS;
        }
        rbipc_ns_to_timespec(remaining_ns, out_ts);
    } else {
        out_ts->tv_sec = 0;
        out_ts->tv_nsec = (long)RBIPC_FUTEX_PERIOD_NS;
    }
    return 0;
}

/**
 * @brief Increment active waiter counter using sequential consistency.
 *
 * @param[in,out] futex_waiters Atomic counter in shared memory.
 */
RBIPC_INLINE void rbipc_sync_register_waiter(_Atomic uint32_t *futex_waiters) {
    if (futex_waiters) {
        atomic_fetch_add_explicit(futex_waiters, 1, memory_order_seq_cst);
    }
}

/**
 * @brief Decrement active waiter counter using sequential consistency.
 *
 * @param[in,out] futex_waiters Atomic counter in shared memory.
 */
RBIPC_INLINE void rbipc_sync_deregister_waiter(_Atomic uint32_t *futex_waiters) {
    if (futex_waiters) {
        atomic_fetch_sub_explicit(futex_waiters, 1, memory_order_seq_cst);
    }
}

RBIPC_LEAF
int rbipc_sync_backoff(rbipc_sync_state_t * RBIPC_RESTRICT state, _Atomic uint32_t *futex_word,
                       _Atomic uint32_t *futex_waiters) {
    if (RBIPC_UNLIKELY(!state || !futex_word)) {
        return RBIPC_ERR_INVAL;
    }

    uint64_t now = rbipc_clock_monotonic_ns();
    if (RBIPC_UNLIKELY(rbipc_sync_is_expired(state, now))) {
        return RBIPC_ERR_TIMEOUT;
    }

    struct timespec ts;
    int rc = rbipc_sync_calc_timeout_spec(state, now, &ts);
    if (RBIPC_UNLIKELY(rc != 0)) {
        return rc;
    }

    rbipc_sync_register_waiter(futex_waiters);
    uint32_t current_val = atomic_load_explicit(futex_word, memory_order_relaxed);
    rbipc_futex_wait(futex_word, current_val, &ts);
    rbipc_sync_deregister_waiter(futex_waiters);

    state->spin_count = 0;
    return 0;
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
    if (RBIPC_UNLIKELY(!futex_word)) return;
    atomic_fetch_add_explicit(futex_word, 1, memory_order_release);
    if (rbipc_sync_should_wake(futex_waiters)) {
        rbipc_futex_wake(futex_word, count);
    }
}

RBIPC_LEAF
void rbipc_sync_wake_one(_Atomic uint32_t *futex_word, _Atomic uint32_t *futex_waiters) {
    rbipc_sync_dispatch_wake(futex_word, futex_waiters, 1);
}

RBIPC_LEAF
void rbipc_sync_wake_all(_Atomic uint32_t *futex_word, _Atomic uint32_t *futex_waiters) {
    rbipc_sync_dispatch_wake(futex_word, futex_waiters, INT32_MAX);
}
