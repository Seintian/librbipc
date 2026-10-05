/**
 * @file rbipc_sync.h
 * @brief Hybrid 3-tier backoff synchronization engine
 */

#ifndef RBIPC_SYNC_H
#define RBIPC_SYNC_H

#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>

#define RBIPC_DEFAULT_SPIN_PAUSE 2000
#define RBIPC_DEFAULT_SPIN_YIELD 2050

typedef struct {
    uint32_t spin_count;
    uint64_t deadline_ns;
    bool has_deadline;
} rbipc_sync_state_t;

/**
 * @brief Initialize sync state with an optional timeout in nanoseconds.
 *
 * @param state Pointer to sync state.
 * @param timeout_ns Timeout in nanoseconds (0 for non-blocking check, UINT64_MAX for infinite).
 */
void rbipc_sync_state_init(rbipc_sync_state_t *state, uint64_t timeout_ns);

/**
 * @brief Execute one backoff step according to the 3-tier hybrid strategy:
 *   Tier 1: Hardware CPU pause
 *   Tier 2: Cooperative sched_yield()
 *   Tier 3: sys_futex sleep
 *
 * @param state Current sync state.
 * @param futex_word Futex sequence atomic counter.
 * @param futex_waiters Optional atomic counter of threads sleeping in futex.
 * @return 0 on continued wait, RBIPC_ERR_TIMEOUT if deadline exceeded.
 */
int rbipc_sync_backoff(rbipc_sync_state_t *state, _Atomic uint32_t *futex_word,
                       _Atomic uint32_t *futex_waiters);

/**
 * @brief Wake one waiter on the given futex word after state changes.
 *
 * Checks futex_waiters before invoking the kernel futex syscall to avoid
 * expensive context switches when no threads are sleeping.
 *
 * @param futex_word Futex sequence atomic counter.
 * @param futex_waiters Optional atomic counter of threads sleeping in futex.
 */
void rbipc_sync_wake_one(_Atomic uint32_t *futex_word, _Atomic uint32_t *futex_waiters);

/**
 * @brief Wake all waiters on the given futex word (e.g. for shutdown).
 *
 * @param futex_word Futex sequence atomic counter.
 * @param futex_waiters Optional atomic counter of threads sleeping in futex.
 */
void rbipc_sync_wake_all(_Atomic uint32_t *futex_word, _Atomic uint32_t *futex_waiters);

#endif /* RBIPC_SYNC_H */
