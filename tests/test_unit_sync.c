/**
 * @file test_unit_sync.c
 * @brief Unit tests for sync backoff, timeouts, and futex waking
 */

#include "rbipc.h"
#include "../src/rbipc_sync.h"
#include "../src/rbipc_futex.h"
#include <stdio.h>
#include <assert.h>
#include <errno.h>
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

    /* Explicit asymmetric barrier invocation */
    rbipc_sync_asymmetric_barrier();

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

static bool dummy_pred_true(void *arg) {
    (void)arg;
    return true;
}

static bool dummy_pred_countdown(void *arg) {
    int *count = (int *)arg;
    if (*count > 0) {
        (*count)--;
        return false;
    }
    return true;
}

static void test_adaptive_spin_backoff(void) {
    _Atomic uint32_t futex_word;
    atomic_init(&futex_word, 0);
    _Atomic uint32_t futex_waiters;
    atomic_init(&futex_waiters, 0);

    rbipc_sync_state_t state;
    rbipc_sync_state_init(&state, 1);

    /* Spin pause check */
    rbipc_sync_spin_pause(32);

    /* Adaptive backoff with predicate returning true immediately */
    int rc = rbipc_sync_backoff_adaptive(&state, &futex_word, &futex_waiters,
                                         RBIPC_WAIT_ADAPTIVE, 64, dummy_pred_true, NULL);
    assert(rc == 0);
    /* No waiter registered or left behind */
    assert(atomic_load(&futex_waiters) == 0);

    /* Adaptive backoff with countdown predicate satisfying within spin budget */
    int countdown = 10;
    rc = rbipc_sync_backoff_adaptive(&state, &futex_word, &futex_waiters,
                                     RBIPC_WAIT_ADAPTIVE, 64, dummy_pred_countdown, &countdown);
    assert(rc == 0);
    assert(countdown == 0);
    assert(atomic_load(&futex_waiters) == 0);

    /* Adaptive backoff with NULL predicate falls back to standard backoff (immediate timeout) */
    rbipc_sync_state_init(&state, 1);
    while (rc == 0) {
        rc = rbipc_sync_backoff_adaptive(&state, &futex_word, &futex_waiters,
                                         RBIPC_WAIT_ADAPTIVE, 64, NULL, NULL);
    }
    assert(rc == RBIPC_ERR_TIMEOUT);

    /* Zero-spin policy ignores predicate and falls back to futex wait */
    rbipc_sync_state_init(&state, 1);
    rc = 0;
    while (rc == 0) {
        rc = rbipc_sync_backoff_adaptive(&state, &futex_word, &futex_waiters,
                                         RBIPC_WAIT_ZERO_SPIN, 64, dummy_pred_true, NULL);
    }
    assert(rc == RBIPC_ERR_TIMEOUT);

    /* NULL error checking */
    assert(rbipc_sync_backoff_adaptive(NULL, &futex_word, &futex_waiters,
                                       RBIPC_WAIT_ADAPTIVE, 64, dummy_pred_true, NULL) == RBIPC_ERR_INVAL);
    assert(rbipc_sync_backoff_adaptive(&state, NULL, &futex_waiters,
                                       RBIPC_WAIT_ADAPTIVE, 64, dummy_pred_true, NULL) == RBIPC_ERR_INVAL);
}

static void test_wait_policy_api(void) {
    rbipc_ring_t *ring = NULL;
    int rc = rbipc_create_ex(NULL, 1024, 64, RBIPC_CREATE_EXCL, 0660, &ring);
    assert(rc == RBIPC_OK);
    assert(ring != NULL);

    /* Default must be ZERO_SPIN */
    assert(rbipc_get_wait_policy(ring) == RBIPC_WAIT_ZERO_SPIN);

    /* Change to ADAPTIVE */
    assert(rbipc_set_wait_policy(ring, RBIPC_WAIT_ADAPTIVE) == RBIPC_OK);
    assert(rbipc_get_wait_policy(ring) == RBIPC_WAIT_ADAPTIVE);

    /* Invalid policy */
    assert(rbipc_set_wait_policy(ring, 0x00FFU) == RBIPC_ERR_INVAL);
    assert(rbipc_get_wait_policy(ring) == RBIPC_WAIT_ADAPTIVE);

    /* Change back to ZERO_SPIN */
    assert(rbipc_set_wait_policy(ring, RBIPC_WAIT_ZERO_SPIN) == RBIPC_OK);
    assert(rbipc_get_wait_policy(ring) == RBIPC_WAIT_ZERO_SPIN);

    /* NULL safety */
    assert(rbipc_set_wait_policy(NULL, RBIPC_WAIT_ADAPTIVE) == RBIPC_ERR_INVAL);
    assert(rbipc_get_wait_policy(NULL) == RBIPC_WAIT_ZERO_SPIN);

    /* Test write and read with adaptive policy */
    assert(rbipc_set_wait_policy(ring, RBIPC_WAIT_ADAPTIVE) == RBIPC_OK);
    void *wbuf = NULL;
    uint64_t wt = 0;
    rc = rbipc_reserve_write(ring, 16, &wbuf, &wt);
    assert(rc == RBIPC_OK);
    assert(wbuf != NULL);
    rc = rbipc_commit_write(ring, wt, 16);
    assert(rc == RBIPC_OK);

    const void *rbuf = NULL;
    uint32_t rlen = 0;
    uint64_t rt = 0;
    rc = rbipc_read_acquire(ring, &rbuf, &rlen, &rt);
    assert(rc == RBIPC_OK);
    assert(rlen == 16);
    assert(rt == wt);
    rc = rbipc_read_release(ring, rt);
    assert(rc == RBIPC_OK);

    rbipc_detach(ring);
}

static void test_hardware_wait(void) {
    bool has_waitpkg = rbipc_cpu_has_waitpkg();
    bool has_wfxt = rbipc_cpu_has_wfxt();
    printf("  [test_hardware_wait] CPU features: waitpkg=%d, wfxt=%d\n",
           (int)has_waitpkg, (int)has_wfxt);

    _Atomic uint64_t dummy_word;
    atomic_init(&dummy_word, 42);

    /* Invalid pointer check */
    assert(rbipc_sync_hardware_wait(NULL, 1000, NULL, NULL) == RBIPC_ERR_INVAL);

    /* Zero timeout with no satisfied predicate */
    assert(rbipc_sync_hardware_wait(&dummy_word, 0, NULL, NULL) == RBIPC_ERR_TIMEOUT);

    /* Immediate predicate match */
    assert(rbipc_sync_hardware_wait(&dummy_word, 1000, dummy_pred_true, NULL) == RBIPC_OK);

    /* Non-satisfied predicate returns OK if satisfied, or timeout/ENOSYS depending on CPU support */
    int countdown = 3;
    int rc_pred = rbipc_sync_hardware_wait(&dummy_word, 1000, dummy_pred_countdown, &countdown);
    if (has_waitpkg || has_wfxt) {
        assert(rc_pred == RBIPC_OK || rc_pred == RBIPC_ERR_TIMEOUT);
    } else {
        assert(rc_pred == -ENOSYS);
    }

    /* Hardware wait execution or fallback behavior without predicate */
    int rc = rbipc_sync_hardware_wait(&dummy_word, 500, NULL, NULL);
    if (has_waitpkg || has_wfxt) {
        assert(rc == RBIPC_ERR_TIMEOUT);
    } else {
        assert(rc == -ENOSYS);
    }
}

static void test_futex2_waitv(void) {
    bool has_waitv = rbipc_futex_has_waitv();
    printf("  [test_futex2_waitv] Kernel sys_futex_waitv support: %d\n", (int)has_waitv);

    /* Parameter validation */
    assert(rbipc_futex_waitv(NULL, 1, 1000) == -EINVAL);
    struct futex_waitv w_dummy;
    assert(rbipc_futex_waitv(&w_dummy, 0, 1000) == -EINVAL);
    assert(rbipc_futex_waitv(&w_dummy, 129, 1000) == -EINVAL);

    /* Value mismatch immediate return */
    _Atomic uint32_t fword = 100;
    struct futex_waitv w_test = {
        .val = 99, /* Mismatch with actual 100 */
        .uaddr = (uintptr_t)&fword,
        .flags = FUTEX2_SIZE_U32,
        .__reserved = 0,
    };
    int rc = rbipc_futex_waitv(&w_test, 1, 1000000);
    if (has_waitv) {
        assert(rc == -EAGAIN || rc == -EWOULDBLOCK);
    } else {
        assert(rc == -ENOSYS);
    }

    /* Timeout test */
    w_test.val = 100; /* Matching value */
    rc = rbipc_futex_waitv(&w_test, 1, 50000); /* 50 us */
    if (has_waitv) {
        assert(rc == -ETIMEDOUT);
    } else {
        assert(rc == -ENOSYS);
    }

    /* Test rbipc_futex_wait_or_shutdown */
    assert(rbipc_futex_wait_or_shutdown(NULL, 100, NULL, 1000) == RBIPC_ERR_INVAL);
    _Atomic uint32_t sflag = 0;
    /* Immediate return on mismatch */
    assert(rbipc_futex_wait_or_shutdown(&fword, 99, &sflag, 1000000) == RBIPC_OK);
    /* Timeout */
    assert(rbipc_futex_wait_or_shutdown(&fword, 100, &sflag, 50000) == RBIPC_ERR_TIMEOUT);

    /* Test rbipc_futex2_wait_slot_or_shutdown */
    _Atomic uint64_t slot_seq = 200;
    assert(rbipc_futex2_wait_slot_or_shutdown(NULL, 200, &sflag, 1000) == RBIPC_ERR_INVAL);
    /* Immediate return on mismatch */
    assert(rbipc_futex2_wait_slot_or_shutdown(&slot_seq, 199, &sflag, 1000000) == RBIPC_OK);
    /* Timeout */
    assert(rbipc_futex2_wait_slot_or_shutdown(&slot_seq, 200, &sflag, 50000) == RBIPC_ERR_TIMEOUT);

    /* Test rbipc_sync_wait_registered_or_shutdown */
    _Atomic uint32_t sync_waiters = 0;
    rbipc_sync_state_t state;
    rbipc_sync_state_init(&state, 1); /* 1ns */
    rbipc_sync_register_waiter(&sync_waiters);
    assert(atomic_load(&sync_waiters) == 1);
    rc = rbipc_sync_wait_registered_or_shutdown(&state, &fword, &sync_waiters, &sflag);
    while (rc == 0) {
        rbipc_sync_register_waiter(&sync_waiters);
        rc = rbipc_sync_wait_registered_or_shutdown(&state, &fword, &sync_waiters, &sflag);
    }
    assert(rc == RBIPC_ERR_TIMEOUT);
    assert(atomic_load(&sync_waiters) == 0);

    /* NULL validation */
    assert(rbipc_sync_wait_registered_or_shutdown(NULL, &fword, &sync_waiters, &sflag) == RBIPC_ERR_INVAL);
    assert(rbipc_sync_wait_registered_or_shutdown(&state, NULL, &sync_waiters, &sflag) == RBIPC_ERR_INVAL);
}

static void test_futex_wake_op(void) {
    /* Parameter validation */
    _Atomic uint32_t u1 = 0;
    _Atomic uint32_t u2 = 10;
    assert(rbipc_futex_wake_op(NULL, 1, &u2, 1, FUTEX_OP_ADD, 1, FUTEX_OP_CMP_GT, 0) == -EINVAL);
    assert(rbipc_futex_wake_op(&u1, 1, NULL, 1, FUTEX_OP_ADD, 1, FUTEX_OP_CMP_GT, 0) == -EINVAL);

    /* Atomic secondary modification with conditional wake */
    int rc = rbipc_futex_wake_op(&u1, 0, &u2, 1, FUTEX_OP_ADD, 5, FUTEX_OP_CMP_GT, 0);
#if defined(__linux__)
    assert(rc >= 0);
    /* u2 was 10, added 5 -> should now be 15 */
    assert(atomic_load(&u2) == 15);
#else
    assert(rc == -ENOSYS);
#endif
}

int main(void) {
    printf("[test_unit_sync] Running tests...\n");
    test_sync_state();
    test_sync_backoff_and_timeout();
    test_asymmetric_waiter_registration();
    test_adaptive_spin_backoff();
    test_wait_policy_api();
    test_hardware_wait();
    test_futex2_waitv();
    test_futex_wake_op();
    printf("[test_unit_sync] All tests passed!\n");
    return 0;
}
