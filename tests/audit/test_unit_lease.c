/**
 * @file test_unit_lease.c
 * @brief Unit tests for consumer lease timeouts and head-of-line blocking recovery.
 */

#include "rbipc.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <time.h>

static void sleep_ms(int ms) {
    struct timespec ts = {
        .tv_sec = ms / 1000,
        .tv_nsec = (ms % 1000) * 1000000L
    };
    nanosleep(&ts, NULL);
}

static void test_lease_getter_setter(void) {
    rbipc_ring_t *ring = NULL;
    int rc = rbipc_create(NULL, 16, 256, &ring);
    assert(rc == RBIPC_OK);
    assert(ring != NULL);

    /* Initial lease timeout must be 0 (disabled) */
    assert(rbipc_get_lease_timeout(ring) == 0);

    /* Set lease timeout to 50ms */
    uint64_t timeout_ns = (long long)50 * 1000 * 1000ULL;
    rc = rbipc_set_lease_timeout(ring, timeout_ns);
    assert(rc == RBIPC_OK);
    assert(rbipc_get_lease_timeout(ring) == timeout_ns);

    /* Reset to 0 */
    rc = rbipc_set_lease_timeout(ring, 0);
    assert(rc == RBIPC_OK);
    assert(rbipc_get_lease_timeout(ring) == 0);

    rbipc_detach(ring);
}

static void test_lease_revocation_proactive(void) {
    rbipc_ring_t *ring = NULL;
    int rc = rbipc_create(NULL, 16, 256, &ring);
    assert(rc == RBIPC_OK);

    /* Set short lease timeout of 20 ms */
    uint64_t lease_ns = (long long)20 * 1000 * 1000ULL;
    assert(rbipc_set_lease_timeout(ring, lease_ns) == RBIPC_OK);

    /* Write 1 message */
    void *wbuf = NULL;
    uint64_t wticket = 0;
    rc = rbipc_reserve_write(ring, 64, &wbuf, &wticket);
    assert(rc == RBIPC_OK);
    memset(wbuf, 0xAA, 64);
    rc = rbipc_commit_write(ring, wticket, 64);
    assert(rc == RBIPC_OK);

    /* Consumer acquires slot */
    const void *rbuf = NULL;
    uint32_t rlen = 0;
    uint64_t rticket = 0;
    rc = rbipc_read_acquire(ring, &rbuf, &rlen, &rticket);
    assert(rc == RBIPC_OK);
    assert(rticket == wticket);

    /* Before timeout expires, audit should find 0 expired leases */
    assert(rbipc_revoke_expired_leases(ring) == 0);

    /* Sleep longer than lease duration (35 ms) */
    sleep_ms(35);

    /* Now audit must detect and revoke the expired lease */
    int revoked = rbipc_revoke_expired_leases(ring);
    assert(revoked == 1);

    /* Consumer attempts to release already-revoked slot -> must fail with RBIPC_ERR_TIMEOUT */
    rc = rbipc_read_release(ring, rticket);
    assert(rc == RBIPC_ERR_TIMEOUT);

    /* Verify revoked slot was recycled to target sequence:
     * Write remaining 15 slots (tickets 1..15), then write ticket 16 which reuses slot 0. */
    for (int i = 1; i < 16; ++i) {
        void *fill_buf = NULL;
        uint64_t fill_ticket = 0;
        rc = rbipc_reserve_write(ring, 32, &fill_buf, &fill_ticket);
        assert(rc == RBIPC_OK);
        assert(fill_ticket == (uint64_t)i);
        rc = rbipc_commit_write(ring, fill_ticket, 32);
        assert(rc == RBIPC_OK);
    }

    /* Ticket 16 maps to index (16 & 15) == 0 (slot 0). Must be vacant and claimable. */
    void *next_buf = NULL;
    uint64_t next_ticket = 0;
    rc = rbipc_reserve_write(ring, 32, &next_buf, &next_ticket);
    assert(rc == RBIPC_OK);
    assert(next_ticket == 16);
    rc = rbipc_commit_write(ring, next_ticket, 32);
    assert(rc == RBIPC_OK);

    rbipc_detach(ring);
}

static void test_lease_auto_recovery_on_write(void) {
    /* Capacity 2 ring buffer to trigger quick full pipeline */
    rbipc_ring_t *ring = NULL;
    int rc = rbipc_create(NULL, 2, 256, &ring);
    assert(rc == RBIPC_OK);

    /* Configure lease timeout of 20 ms */
    uint64_t lease_ns = (long long)20 * 1000 * 1000ULL;
    assert(rbipc_set_lease_timeout(ring, lease_ns) == RBIPC_OK);

    /* Fill ring: write slot 0 and slot 1 */
    void *wbuf = NULL;
    uint64_t t0 = 0, t1 = 0;
    rc = rbipc_reserve_write(ring, 32, &wbuf, &t0);
    assert(rc == RBIPC_OK);
    rc = rbipc_commit_write(ring, t0, 32);
    assert(rc == RBIPC_OK);

    rc = rbipc_reserve_write(ring, 32, &wbuf, &t1);
    assert(rc == RBIPC_OK);
    rc = rbipc_commit_write(ring, t1, 32);
    assert(rc == RBIPC_OK);

    /* Consumer acquires slot 0, then stalls without releasing */
    const void *rbuf = NULL;
    uint32_t rlen = 0;
    uint64_t rticket = 0;
    rc = rbipc_read_acquire(ring, &rbuf, &rlen, &rticket);
    assert(rc == RBIPC_OK);
    assert(rticket == t0);

    /* Consumer stalls for 30 ms */
    sleep_ms(30);

    /* Ring is full (slot 0 acquired by stalled consumer, slot 1 committed).
     * Producer attempts to reserve write for ticket 2 (which needs slot 0).
     * Producer backoff must auto-revoke slot 0 expired lease and claim the slot. */
    uint64_t t2 = 0;
    rc = rbipc_reserve_write(ring, 32, &wbuf, &t2);
    assert(rc == RBIPC_OK);
    assert(t2 == 2);
    rc = rbipc_commit_write(ring, t2, 32);
    assert(rc == RBIPC_OK);

    /* When stalled consumer finally wakes up and calls release on slot 0, it must fail */
    rc = rbipc_read_release(ring, rticket);
    assert(rc == RBIPC_ERR_TIMEOUT);

    rbipc_detach(ring);
}

int main(void) {
    printf("[test_unit_lease] Running consumer lease timeout tests...\n");
    test_lease_getter_setter();
    test_lease_revocation_proactive();
    test_lease_auto_recovery_on_write();
    printf("[test_unit_lease] All tests passed successfully.\n");
    return 0;
}
