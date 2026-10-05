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
    assert(state.spin_count == 0);
    assert(state.has_deadline == false);

    rbipc_sync_state_init(&state, 1000000ULL); /* 1ms */
    assert(state.spin_count == 0);
    assert(state.has_deadline == true);
    assert(state.deadline_ns > 0);

    /* NULL safety */
    rbipc_sync_state_init(NULL, 0);
}

static void test_sync_backoff_and_timeout(void) {
    _Atomic uint32_t futex_word;
    atomic_init(&futex_word, 0);

    rbipc_sync_state_t state;
    /* Timeout of 1 nanosecond to trigger immediate expiration */
    rbipc_sync_state_init(&state, 1);

    int rc = rbipc_sync_backoff(&state, &futex_word);
    /* Should either spin once or immediately timeout */
    while (rc == 0) {
        rc = rbipc_sync_backoff(&state, &futex_word);
    }
    assert(rc == RBIPC_ERR_TIMEOUT);

    /* NULL argument checks */
    assert(rbipc_sync_backoff(NULL, &futex_word) == RBIPC_ERR_INVAL);
    assert(rbipc_sync_backoff(&state, NULL) == RBIPC_ERR_INVAL);

    /* Waking */
    rbipc_sync_wake_one(&futex_word);
    assert(atomic_load(&futex_word) == 1);

    rbipc_sync_wake_all(&futex_word);
    assert(atomic_load(&futex_word) == 2);

    /* Safe NULL wake */
    rbipc_sync_wake_one(NULL);
    rbipc_sync_wake_all(NULL);
}

int main(void) {
    printf("[test_unit_sync] Running tests...\n");
    test_sync_state();
    test_sync_backoff_and_timeout();
    printf("[test_unit_sync] All tests passed!\n");
    return 0;
}
