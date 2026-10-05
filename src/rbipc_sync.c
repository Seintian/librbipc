/**
 * @file rbipc_sync.c
 * @brief Implementation of 3-tier hybrid backoff synchronization
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "rbipc.h"
#include "rbipc_sync.h"
#include "rbipc_arch.h"
#include "rbipc_futex.h"

#include <sched.h>
#include <errno.h>

void rbipc_sync_state_init(rbipc_sync_state_t *state, uint64_t timeout_ns) {
    if (!state) return;
    state->spin_count = 0;
    if (timeout_ns == UINT64_MAX) {
        state->has_deadline = false;
        state->deadline_ns = 0;
    } else {
        state->has_deadline = true;
        state->deadline_ns = rbipc_clock_monotonic_ns() + timeout_ns;
    }
}

int rbipc_sync_backoff(rbipc_sync_state_t *state, _Atomic uint32_t *futex_word,
                       _Atomic uint32_t *futex_waiters) {
    if (!state || !futex_word) return RBIPC_ERR_INVAL;

    if (state->has_deadline) {
        uint64_t now = rbipc_clock_monotonic_ns();
        if (now >= state->deadline_ns) {
            return RBIPC_ERR_TIMEOUT;
        }
    }

    state->spin_count++;
    if (state->spin_count < RBIPC_DEFAULT_SPIN_PAUSE) {
        rbipc_cpu_pause();
        return 0;
    } else if (state->spin_count < RBIPC_DEFAULT_SPIN_YIELD) {
        sched_yield();
        return 0;
    }

    /* Tier 3: Futex Sleep */
    struct timespec ts;
    struct timespec *pts = &ts;

    if (state->has_deadline) {
        uint64_t now = rbipc_clock_monotonic_ns();
        if (now >= state->deadline_ns) {
            return RBIPC_ERR_TIMEOUT;
        }
        uint64_t remaining_ns = state->deadline_ns - now;
        /* Cap futex wait duration at 20ms so we regularly check shutdown/peer status */
        if (remaining_ns > 20000000ULL) {
            remaining_ns = 20000000ULL;
        }
        rbipc_ns_to_timespec(remaining_ns, &ts);
    } else {
        /* Default 20ms periodic wakeup */
        ts.tv_sec = 0;
        ts.tv_nsec = 20000000L;
    }

    if (futex_waiters) {
        atomic_fetch_add_explicit(futex_waiters, 1, memory_order_seq_cst);
    }

    uint32_t current_val = atomic_load_explicit(futex_word, memory_order_relaxed);
    rbipc_futex_wait(futex_word, current_val, pts);

    if (futex_waiters) {
        atomic_fetch_sub_explicit(futex_waiters, 1, memory_order_seq_cst);
    }

    state->spin_count = 0;
    return 0;
}

void rbipc_sync_wake_one(_Atomic uint32_t *futex_word, _Atomic uint32_t *futex_waiters) {
    if (!futex_word) return;
    atomic_fetch_add_explicit(futex_word, 1, memory_order_release);
    if (!futex_waiters || atomic_load_explicit(futex_waiters, memory_order_seq_cst) > 0) {
        rbipc_futex_wake(futex_word, 1);
    }
}

void rbipc_sync_wake_all(_Atomic uint32_t *futex_word, _Atomic uint32_t *futex_waiters) {
    if (!futex_word) return;
    atomic_fetch_add_explicit(futex_word, 1, memory_order_release);
    if (!futex_waiters || atomic_load_explicit(futex_waiters, memory_order_seq_cst) > 0) {
        rbipc_futex_wake(futex_word, INT32_MAX);
    }
}
