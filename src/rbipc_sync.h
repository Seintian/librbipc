/**
 * @file rbipc_sync.h
 * @brief Zero-spin passive IPC synchronization engine with Linux futex
 */

#ifndef RBIPC_SYNC_H
#define RBIPC_SYNC_H

#include "rbipc.h"
#include "rbipc_attr.h"

#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>

#define RBIPC_DEFAULT_SPIN_PAUSE 0
#define RBIPC_DEFAULT_SPIN_YIELD 0
#define RBIPC_FUTEX_PERIOD_NS    20000000ULL /* 20ms capped wait to audit shutdown & peer status */

/**
 * @struct rbipc_sync_state_t
 * @brief Thread-local state tracking for backoff and deadline management.
 */
typedef struct {
    uint32_t spin_count;    /**< Legacy spin counter (zero in passive mode) */
    uint64_t deadline_ns;   /**< Monotonic timestamp deadline in nanoseconds */
    bool has_deadline;      /**< True if bounded timeout is active */
} rbipc_sync_state_t;

/**
 * @brief Initialize sync state with an optional timeout in nanoseconds.
 *
 * @param state Pointer to sync state.
 * @param timeout_ns Timeout in nanoseconds (0 for non-blocking check, UINT64_MAX for infinite).
 */
void rbipc_sync_state_init(rbipc_sync_state_t *state, uint64_t timeout_ns);

/**
 * @brief Execute passive wait step:
 *   Zero active spinning, immediate suspension via sys_futex sleep.
 *
 * @param state Current sync state.
 * @param futex_word Futex sequence atomic counter.
 * @param futex_waiters Optional atomic counter of threads sleeping in futex.
 * @return 0 on continued wait, RBIPC_ERR_TIMEOUT if deadline exceeded.
 */
RBIPC_NODISCARD
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
