/**
 * @file test_unit_sync.c
 * @brief Unit tests for sync backoff, timeouts, and futex waking
 */

#include "rbipc.h"
#include "../src/rbipc_sync.h"
#include "../src/rbipc_futex.h"
#include <stdio.h>
#include <assert.h>
#include <stdatomic.h>

static void test_sync_state(void) {
    rbipc_sync_state_t state;
    rbipc_sync_state_init(&state, UINT64_MAX);
    assert(state.has_deadline == false);

    rbipc_sync_state_init(&state, RBIPC_NSEC_PER_MSEC); /* 1ms */
    assert(state.has_deadline == true);
    assert(state.deadline_ns > 0);

    /* NULL safety */
    rbipc_sync_state_init(NULL, 0);
}

static void test_asymmetric_waiter_registration(void) {
    _Atomic uint32_t waiters;
    atomic_init(&waiters, 0);

    rbipc_sync_register_waiter(&waiters);
    assert(atomic_load(&waiters) == 1);
    rbipc_sync_register_waiter(&waiters);
    assert(atomic_load(&waiters) == 2);

    rbipc_sync_deregister_waiter(&waiters);
    assert(atomic_load(&waiters) == 1);
    rbipc_sync_deregister_waiter(&waiters);
    assert(atomic_load(&waiters) == 0);

    /* Test wait registered timeout */
    _Atomic uint32_t futex_word;
    atomic_init(&futex_word, 0);
    rbipc_sync_state_t state;
    rbipc_sync_state_init(&state, 1); /* 1ns timeout */

    rbipc_sync_register_waiter(&waiters);
    assert(atomic_load(&waiters) == 1);
    int rc = rbipc_sync_wait_registered(&state, &futex_word, &waiters);
    while (rc == 0) {
        rbipc_sync_register_waiter(&waiters);
        rc = rbipc_sync_wait_registered(&state, &futex_word, &waiters);
    }
    assert(rc == RBIPC_ERR_TIMEOUT);
    /* Upon timeout return, waiter must be deregistered */
    assert(atomic_load(&waiters) == 0);
}

static void test_sync_backoff_and_timeout(void) {
    _Atomic uint32_t futex_word;
    atomic_init(&futex_word, 0);
    _Atomic uint32_t futex_waiters;
    atomic_init(&futex_waiters, 0);

    rbipc_sync_state_t state;
    /* Timeout of 1 nanosecond to trigger immediate expiration */
    rbipc_sync_state_init(&state, 1);

    int rc = rbipc_sync_backoff(&state, &futex_word, &futex_waiters);
    /* Should either wait or immediately timeout */
    while (rc == 0) {
        rc = rbipc_sync_backoff(&state, &futex_word, &futex_waiters);
    }
    assert(rc == RBIPC_ERR_TIMEOUT);

    /* NULL argument checks */
    assert(rbipc_sync_backoff(NULL, &futex_word, &futex_waiters) == RBIPC_ERR_INVAL);
    assert(rbipc_sync_backoff(&state, NULL, &futex_waiters) == RBIPC_ERR_INVAL);
    /* Backoff with NULL waiters should also work */
    rbipc_sync_state_init(&state, 1);
    while (rc == 0) {
        rc = rbipc_sync_backoff(&state, &futex_word, NULL);
    }

    /* Waking without waiters (conditional bypass) */
    rbipc_sync_wake_one(&futex_word, &futex_waiters);
    assert(atomic_load(&futex_word) == 1);

    rbipc_sync_wake_all(&futex_word, &futex_waiters);
    assert(atomic_load(&futex_word) == 2);

    /* Waking with simulated waiters > 0 */
    atomic_store(&futex_waiters, 2);
    rbipc_sync_wake_one(&futex_word, &futex_waiters);
    assert(atomic_load(&futex_word) == 3);
    rbipc_sync_wake_all(&futex_word, &futex_waiters);
    assert(atomic_load(&futex_word) == 4);

    /* Proportional wake_n test */
    rbipc_sync_wake_n(&futex_word, &futex_waiters, 8);
    assert(atomic_load(&futex_word) == 5);

    /* Safe NULL wake */
    rbipc_sync_wake_one(NULL, NULL);
    rbipc_sync_wake_all(NULL, NULL);
    rbipc_sync_wake_n(NULL, NULL, 5);
}

int main(void) {
    printf("[test_unit_sync] Running tests...\n");
    test_sync_state();
    test_sync_backoff_and_timeout();
    test_asymmetric_waiter_registration();
    printf("[test_unit_sync] All tests passed!\n");
    return 0;
}
