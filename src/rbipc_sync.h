/**
 * @file rbipc_sync.h
 * @brief Zero-spin passive IPC synchronization engine based on Linux Fast Userspace Mutexes.
 *
 * @details Implements thread-local deadline tracking and zero-spin futex backoff. Completely
 * eliminates active CPU busy-waiting loops, yielding immediate thread suspension on kernel
 * wait-queues and preserving zero-percent CPU usage during idling or backpressure periods.
 *
 * @author Christian Santarelli
 * @date 2026
 * @copyright Apache License 2.0
 */

#ifndef RBIPC_SYNC_H
#define RBIPC_SYNC_H

#include "rbipc.h"
#include "rbipc_attr.h"

#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>

/**
 * @def RBIPC_DEFAULT_SPIN_PAUSE
 * @brief Number of CPU pause iterations in passive mode (strictly 0).
 */
#define RBIPC_DEFAULT_SPIN_PAUSE 0

/**
 * @def RBIPC_DEFAULT_SPIN_YIELD
 * @brief Number of sched_yield iterations in passive mode (strictly 0).
 */
#define RBIPC_DEFAULT_SPIN_YIELD 0

/**
 * @def RBIPC_FUTEX_PERIOD_NS
 * @brief Capped maximum sleep duration per futex wait invocation (20 milliseconds).
 * @details Ensures periodic wakeups to audit peer crash liveness and shutdown status.
 */
#define RBIPC_FUTEX_PERIOD_NS    20000000ULL

/**
 * @struct rbipc_sync_state_t
 * @brief Thread-local state tracking for backoff progression and deadline timeout management.
 */
typedef struct {
    uint32_t spin_count;    /**< Legacy spin counter retained for ABI compatibility (strictly 0 in passive mode). */
    uint64_t deadline_ns;   /**< Monotonic timestamp deadline in nanoseconds (valid when @ref has_deadline is true). */
    bool has_deadline;      /**< Boolean flag indicating if an explicit operation timeout deadline is active. */
} rbipc_sync_state_t;

/**
 * @brief Initialize a thread-local synchronization state object with an optional timeout.
 *
 * @param[out] state      Pointer to synchronization state structure to initialize.
 * @param[in]  timeout_ns Requested timeout in nanoseconds (0 for non-blocking, UINT64_MAX for unbounded).
 */
RBIPC_LEAF
void rbipc_sync_state_init(rbipc_sync_state_t * RBIPC_RESTRICT state, uint64_t timeout_ns);

/**
 * @brief Execute a passive backoff step via Linux sys_futex suspension.
 *
 * @details Evaluates deadline expiration. If time remains, increments the atomic @p futex_waiters
 * counter, calls `SYS_futex` with `FUTEX_WAIT`, and decrements @p futex_waiters upon waking.
 *
 * @param[in,out] state         Thread-local synchronization state tracking deadline.
 * @param[in]     futex_word    Pointer to atomic futex sequence word in shared memory.
 * @param[in]     futex_waiters Pointer to atomic waiter count in shared memory.
 *
 * @return 0 on continued wait / successful wake.
 * @retval RBIPC_ERR_TIMEOUT if deadline timestamp has been exceeded.
 */
RBIPC_NODISCARD RBIPC_LEAF
int rbipc_sync_backoff(rbipc_sync_state_t * RBIPC_RESTRICT state, _Atomic uint32_t *futex_word,
                       _Atomic uint32_t *futex_waiters);

/**
 * @brief Wake one waiting thread on the specified futex word after state changes.
 *
 * @details Checks @p futex_waiters using atomic load with sequential consistency. If 0 waiters
 * are present, the expensive kernel syscall is completely bypassed (0 context switches).
 *
 * @param[in] futex_word    Pointer to atomic futex sequence word in shared memory.
 * @param[in] futex_waiters Pointer to atomic waiter count in shared memory.
 */
RBIPC_LEAF
void rbipc_sync_wake_one(_Atomic uint32_t *futex_word, _Atomic uint32_t *futex_waiters);

/**
 * @brief Wake all waiting threads on the specified futex word (e.g. for shutdown broadcast).
 *
 * @details Dispatches `FUTEX_WAKE` with `INT_MAX` if waiters are currently sleeping.
 *
 * @param[in] futex_word    Pointer to atomic futex sequence word in shared memory.
 * @param[in] futex_waiters Pointer to atomic waiter count in shared memory.
 */
RBIPC_LEAF
void rbipc_sync_wake_all(_Atomic uint32_t *futex_word, _Atomic uint32_t *futex_waiters);

#endif /* RBIPC_SYNC_H */
