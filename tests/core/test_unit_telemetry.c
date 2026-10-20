/**
 * @file test_unit_telemetry.c
 * @brief Unit tests for enhanced ADS telemetry, disaggregated waiters, and slot histogram.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "rbipc.h"
#include "rbipc_audit.h"
#include "rbipc_slot.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#define TEST_RING_NAME "/rbipc_telemetry_test_shm"

static void sleep_ms(int ms) {
    struct timespec ts = {
        .tv_sec = ms / 1000,
        .tv_nsec = (ms % 1000) * 1000000L
    };
    nanosleep(&ts, NULL);
}

static void test_stats_waiter_disaggregation(void) {
    rbipc_destroy(TEST_RING_NAME);

    rbipc_ring_t *ring = NULL;
    int rc = rbipc_create(TEST_RING_NAME, 16, 128, &ring);
    assert(rc == RBIPC_OK);

    rbipc_ring_t *cons = NULL;
    rc = rbipc_attach_ex(TEST_RING_NAME, RBIPC_ROLE_CONSUMER, &cons);
    assert(rc == RBIPC_OK);

    rbipc_stats_t stats;
    rc = rbipc_get_stats(ring, &stats);
    assert(rc == RBIPC_OK);

    assert(stats.capacity == 16);
    assert(stats.slot_size >= 128);
    assert(stats.active_producers == 1);
    assert(stats.active_consumers == 1);
    assert(stats.futex_waiters == 0);
    assert(stats.write_waiters == 0);
    assert(stats.eventfd_waiters == 0);
    assert(stats.backlog == 0);
    assert(stats.lease_timeout_ns == 0);
    assert(stats.is_shutdown == false);

    rbipc_detach(cons);
    rbipc_detach(ring);
    rbipc_destroy(TEST_RING_NAME);
}

static void test_stats_backlog_tracking(void) {
    rbipc_ring_t *ring = NULL;
    int rc = rbipc_create(NULL, 16, 64, &ring);
    assert(rc == RBIPC_OK);

    rbipc_stats_t stats;

    /* Write 5 messages */
    for (int i = 0; i < 5; ++i) {
        char msg[32];
        snprintf(msg, sizeof(msg), "Msg #%d", i);
        rc = rbipc_write(ring, msg, (uint32_t)strlen(msg) + 1);
        assert(rc == RBIPC_OK);
    }

    rc = rbipc_get_stats(ring, &stats);
    assert(rc == RBIPC_OK);
    assert(stats.write_ticket == 5);
    assert(stats.read_ticket == 0);
    assert(stats.backlog == 5);

    /* Read 2 messages */
    for (int i = 0; i < 2; ++i) {
        char buf[64];
        uint32_t len = 0;
        rc = rbipc_read(ring, buf, sizeof(buf), &len);
        assert(rc == RBIPC_OK);
    }

    rc = rbipc_get_stats(ring, &stats);
    assert(rc == RBIPC_OK);
    assert(stats.write_ticket == 5);
    assert(stats.read_ticket == 2);
    assert(stats.backlog == 3);

    /* Read remaining 3 messages */
    for (int i = 0; i < 3; ++i) {
        char buf[64];
        uint32_t len = 0;
        rc = rbipc_read(ring, buf, sizeof(buf), &len);
        assert(rc == RBIPC_OK);
    }

    rc = rbipc_get_stats(ring, &stats);
    assert(rc == RBIPC_OK);
    assert(stats.write_ticket == 5);
    assert(stats.read_ticket == 5);
    assert(stats.backlog == 0);

    rbipc_detach(ring);
}

static void test_stats_lease_and_cumulative_counters(void) {
    rbipc_ring_t *ring = NULL;
    int rc = rbipc_create(NULL, 16, 64, &ring);
    assert(rc == RBIPC_OK);

    /* Set lease timeout to 50ms */
    uint64_t lease_ns = (long long)50 * 1000 * 1000ULL;
    assert(rbipc_set_lease_timeout(ring, lease_ns) == RBIPC_OK);

    rbipc_stats_t stats;
    assert(rbipc_get_stats(ring, &stats) == RBIPC_OK);
    assert(stats.lease_timeout_ns == lease_ns);
    assert(stats.poisoned_slots_count == 0);
    assert(stats.dead_producers_recovered == 0);
    assert(stats.dead_consumers_recovered == 0);
    assert(stats.leases_revoked_count == 0);
    assert(stats.ticket_holes_healed == 0);

    /* 1. Abort a write to verify poisoned_slots_count */
    void *wbuf = NULL;
    uint64_t wticket = 0;
    rc = rbipc_reserve_write(ring, 32, &wbuf, &wticket);
    assert(rc == RBIPC_OK);
    rc = rbipc_abort_write(ring, wticket);
    assert(rc == RBIPC_OK);

    assert(rbipc_get_stats(ring, &stats) == RBIPC_OK);
    assert(stats.poisoned_slots_count == 1);

    /* Drain the poisoned slot 0 */
    const void *rbuf = NULL;
    uint32_t rlen = 0;
    uint64_t rticket = 0;
    rc = rbipc_read_acquire(ring, &rbuf, &rlen, &rticket);
    assert(rc == RBIPC_ERR_POISONED);
    assert(rticket == 0);
    rc = rbipc_read_release(ring, rticket);
    assert(rc == RBIPC_OK);

    /* 2. Write and acquire a message, let lease expire and revoke */
    rc = rbipc_reserve_write(ring, 32, &wbuf, &wticket);
    assert(rc == RBIPC_OK);
    assert(wticket == 1);
    rc = rbipc_commit_write(ring, wticket, 32);
    assert(rc == RBIPC_OK);

    rc = rbipc_read_acquire(ring, &rbuf, &rlen, &rticket);
    assert(rc == RBIPC_OK);
    assert(rticket == 1);

    /* Sleep longer than lease (60 ms) */
    sleep_ms(60);

    int revoked = rbipc_revoke_expired_leases(ring);
    assert(revoked == 1);

    assert(rbipc_get_stats(ring, &stats) == RBIPC_OK);
    assert(stats.leases_revoked_count == 1);

    rbipc_detach(ring);
}

static void test_audit_recovery_counters(void) {
    rbipc_shm_header_t hdr = {};
    atomic_init(&hdr.futex_seq, 0);
    atomic_init(&hdr.futex_waiters, 0);
    atomic_init(&hdr.write_futex_seq, 0);
    atomic_init(&hdr.write_waiters, 0);
    atomic_init(&hdr.write_ticket, 10);
    atomic_init(&hdr.read_ticket, 0);
    hdr.capacity_mask = 3;
    atomic_init(&hdr.active_producers, 1);
    atomic_init(&hdr.poisoned_slots_count, 0);
    atomic_init(&hdr.dead_producers_recovered, 0);
    atomic_init(&hdr.dead_consumers_recovered, 0);
    atomic_init(&hdr.ticket_holes_healed, 0);
    atomic_init(&hdr.leases_revoked_count, 0);

    rbipc_slot_t slot;
    atomic_init(&slot.sequence, 0);
    atomic_init(&slot.state, RBIPC_SLOT_RESERVED);
    atomic_init(&slot.producer_pid, 9999999); /* Non-existent PID */
    atomic_init(&slot.consumer_pid, 0);
    atomic_init(&slot.acquire_timestamp_ns, 0);

    /* Recover dead producer */
    bool recovered = rbipc_audit_recover_dead_producer(&hdr, &slot, 0);
    assert(recovered == true);
    assert(atomic_load_explicit(&hdr.poisoned_slots_count, memory_order_relaxed) == 1);
    assert(atomic_load_explicit(&hdr.dead_producers_recovered, memory_order_relaxed) == 1);

    /* Recover dead consumer */
    atomic_init(&slot.state, RBIPC_SLOT_COMMITTED);
    atomic_init(&slot.consumer_pid, 9999999);
    atomic_init(&slot.acquire_timestamp_ns, 100);
    bool c_recovered = rbipc_audit_recover_dead_consumer(&hdr, &slot, 0);
    assert(c_recovered == true);
    assert(atomic_load_explicit(&hdr.dead_consumers_recovered, memory_order_relaxed) == 1);

    /* Heal ticket hole */
    rbipc_sync_state_t sync_state;
    rbipc_sync_state_init(&sync_state, 1000000000ULL);
    atomic_init(&slot.state, RBIPC_SLOT_EMPTY);
    atomic_init(&hdr.write_ticket, 5);
    atomic_init(&hdr.active_producers, 0); /* Triggers immediate hole healing */

    bool healed = rbipc_audit_check_and_heal_ticket_hole(&hdr, &slot, 0, &sync_state);
    assert(healed == true);
    assert(atomic_load_explicit(&hdr.poisoned_slots_count, memory_order_relaxed) == 2);
    assert(atomic_load_explicit(&hdr.ticket_holes_healed, memory_order_relaxed) == 1);
}

static void test_slot_stats_histogram(void) {
    rbipc_ring_t *ring = NULL;
    int rc = rbipc_create(NULL, 8, 64, &ring);
    assert(rc == RBIPC_OK);

    rbipc_slot_stats_t ss;
    assert(rbipc_get_slot_stats(NULL, &ss) == RBIPC_ERR_INVAL);
    assert(rbipc_get_slot_stats(ring, NULL) == RBIPC_ERR_INVAL);

    /* Initial state: all 8 slots must be EMPTY */
    assert(rbipc_get_slot_stats(ring, &ss) == RBIPC_OK);
    assert(ss.capacity == 8);
    assert(ss.empty_count == 8);
    assert(ss.reserving_count == 0);
    assert(ss.reserved_count == 0);
    assert(ss.committed_count == 0);
    assert(ss.poisoned_count == 0);
    assert(ss.active_leased_slots == 0);
    assert(ss.max_lease_elapsed_ns == 0);

    /* 1. Reserve one slot (state becomes RESERVED) */
    void *wbuf1 = NULL;
    uint64_t ticket1 = 0;
    rc = rbipc_reserve_write(ring, 32, &wbuf1, &ticket1);
    assert(rc == RBIPC_OK);

    assert(rbipc_get_slot_stats(ring, &ss) == RBIPC_OK);
    assert(ss.empty_count == 7);
    assert(ss.reserved_count == 1);
    assert(ss.committed_count == 0);

    /* 2. Commit slot 1 */
    rc = rbipc_commit_write(ring, ticket1, 32);
    assert(rc == RBIPC_OK);

    assert(rbipc_get_slot_stats(ring, &ss) == RBIPC_OK);
    assert(ss.empty_count == 7);
    assert(ss.reserved_count == 0);
    assert(ss.committed_count == 1);

    /* 3. Reserve and abort slot 2 (state becomes POISONED) */
    void *wbuf2 = NULL;
    uint64_t ticket2 = 0;
    rc = rbipc_reserve_write(ring, 32, &wbuf2, &ticket2);
    assert(rc == RBIPC_OK);
    rc = rbipc_abort_write(ring, ticket2);
    assert(rc == RBIPC_OK);

    assert(rbipc_get_slot_stats(ring, &ss) == RBIPC_OK);
    assert(ss.empty_count == 6);
    assert(ss.committed_count == 1);
    assert(ss.poisoned_count == 1);

    /* 4. Consumer acquires committed slot 1 with a lease enabled */
    assert(rbipc_set_lease_timeout(ring, 100000000ULL) == RBIPC_OK);
    const void *rbuf = NULL;
    uint32_t rlen = 0;
    uint64_t rticket = 0;
    rc = rbipc_read_acquire(ring, &rbuf, &rlen, &rticket);
    assert(rc == RBIPC_OK);

    sleep_ms(5);

    assert(rbipc_get_slot_stats(ring, &ss) == RBIPC_OK);
    assert(ss.active_leased_slots == 1);
    assert(ss.max_lease_elapsed_ns > 0);

    /* Release slot 1 */
    rc = rbipc_read_release(ring, rticket);
    assert(rc == RBIPC_OK);

    assert(rbipc_get_slot_stats(ring, &ss) == RBIPC_OK);
    assert(ss.active_leased_slots == 0);
    assert(ss.empty_count == 7); /* slot 1 recycled back to empty */
    assert(ss.poisoned_count == 1);

    rbipc_detach(ring);
}

static void test_slot_stats_histogram_packed(void) {
    rbipc_ring_t *ring = NULL;
    int rc = rbipc_create_ex(NULL, 8, 64, RBIPC_FLAG_PACKED, 0, &ring);
    assert(rc == RBIPC_OK);

    rbipc_slot_stats_t ss;
    assert(rbipc_get_slot_stats(ring, &ss) == RBIPC_OK);
    assert(ss.capacity == 8);
    assert(ss.empty_count == 8);
    assert(ss.committed_count == 0);
    assert(ss.poisoned_count == 0);

    /* 1. Write a message and commit */
    void *wbuf1 = NULL;
    uint64_t ticket1 = 0;
    rc = rbipc_reserve_write(ring, 32, &wbuf1, &ticket1);
    assert(rc == RBIPC_OK);
    rc = rbipc_commit_write(ring, ticket1, 32);
    assert(rc == RBIPC_OK);

    assert(rbipc_get_slot_stats(ring, &ss) == RBIPC_OK);
    assert(ss.empty_count == 7);
    assert(ss.committed_count == 1);
    assert(ss.poisoned_count == 0);

    /* 2. Abort a write */
    void *wbuf2 = NULL;
    uint64_t ticket2 = 0;
    rc = rbipc_reserve_write(ring, 32, &wbuf2, &ticket2);
    assert(rc == RBIPC_OK);
    rc = rbipc_abort_write(ring, ticket2);
    assert(rc == RBIPC_OK);

    assert(rbipc_get_slot_stats(ring, &ss) == RBIPC_OK);
    assert(ss.empty_count == 6);
    assert(ss.committed_count == 1);
    assert(ss.poisoned_count == 1);

    /* 3. Read and release message 1 */
    const void *rbuf = NULL;
    uint32_t rlen = 0;
    uint64_t rticket = 0;
    rc = rbipc_read_acquire(ring, &rbuf, &rlen, &rticket);
    assert(rc == RBIPC_OK);
    assert(rticket == ticket1);
    rc = rbipc_read_release(ring, rticket);
    assert(rc == RBIPC_OK);

    assert(rbipc_get_slot_stats(ring, &ss) == RBIPC_OK);
    assert(ss.empty_count == 7);
    assert(ss.committed_count == 0);
    assert(ss.poisoned_count == 1);

    rbipc_detach(ring);
}

int main(void) {
    printf("[test_unit_telemetry] Running telemetry enhancement tests...\n");
    test_stats_waiter_disaggregation();
    test_stats_backlog_tracking();
    test_stats_lease_and_cumulative_counters();
    test_audit_recovery_counters();
    test_slot_stats_histogram();
    test_slot_stats_histogram_packed();
    printf("[test_unit_telemetry] All telemetry unit tests passed successfully.\n");
    return 0;
}
