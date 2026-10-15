/**
 * @file test_unit_audit.c
 * @brief Unit tests for peer liveness auditing and crash recovery engine.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "rbipc.h"
#include "../src/rbipc_audit.h"
#include "../src/rbipc_slot.h"

#include <assert.h>
#include <stdio.h>
#include <unistd.h>

int main(void) {
    printf("[test_unit_audit] Running peer auditing and recovery tests...\n");

    /* 1. Liveness tests */
    assert(rbipc_audit_is_peer_alive(0) == false);
    assert(rbipc_audit_is_peer_alive(-1) == false);
    assert(rbipc_audit_is_peer_alive(getpid()) == true);
    assert(rbipc_audit_is_peer_alive(9999999) == false);

    /* 2. Dead producer recovery */
    rbipc_shm_header_t hdr = {};
    atomic_init(&hdr.futex_seq, 0);
    atomic_init(&hdr.futex_waiters, 0);
    atomic_init(&hdr.write_ticket, 10);
    hdr.capacity_mask = 3;
    atomic_init(&hdr.active_producers, 1);

    rbipc_slot_t slot;
    atomic_init(&slot.sequence, 0);
    atomic_init(&slot.state, RBIPC_SLOT_RESERVED);
    atomic_init(&slot.producer_pid, 9999999); /* Non-existent PID */
    atomic_init(&slot.consumer_pid, 0);
    atomic_init(&slot.acquire_timestamp_ns, 0);

    bool recovered = rbipc_audit_recover_dead_producer(&hdr, &slot, 0);
    assert(recovered == true);
    assert(atomic_load_explicit(&slot.state, memory_order_relaxed) == RBIPC_SLOT_POISONED);

    /* 3. Dead consumer recovery */
    atomic_init(&slot.state, RBIPC_SLOT_COMMITTED);
    atomic_init(&slot.consumer_pid, 9999999);
    atomic_init(&slot.acquire_timestamp_ns, 100);
    bool c_recovered = rbipc_audit_recover_dead_consumer(&hdr, &slot, 0);
    assert(c_recovered == true);
    assert(atomic_load_explicit(&slot.state, memory_order_relaxed) == RBIPC_SLOT_EMPTY);

    /* 4. Ticket hole healing */
    rbipc_sync_state_t sync_state;
    rbipc_sync_state_init(&sync_state, 1000000000ULL);
    atomic_init(&slot.state, RBIPC_SLOT_EMPTY);
    atomic_init(&hdr.write_ticket, 5); /* write_ticket is 5, target slot is ticket 0 */
    atomic_init(&hdr.active_producers, 0); /* 0 active producers triggers immediate healing */

    bool healed = rbipc_audit_check_and_heal_ticket_hole(&hdr, &slot, 0, &sync_state);
    assert(healed == true);
    assert(atomic_load_explicit(&slot.state, memory_order_relaxed) == RBIPC_SLOT_POISONED);

    printf("[test_unit_audit] All tests passed!\n");
    return 0;
}