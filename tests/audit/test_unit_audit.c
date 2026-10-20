/**
 * @file test_unit_audit.c
 * @brief Unit tests for peer liveness auditing and crash recovery engine.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "rbipc.h"
#include "rbipc_audit.h"
#include "rbipc_slot.h"

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

    /* 5. Dead producer recovery on packed ring */
    rbipc_shm_header_t packed_hdr = {};
    packed_hdr.flags = htole32(RBIPC_FLAG_PACKED);
    atomic_init(&packed_hdr.futex_seq, 0);
    atomic_init(&packed_hdr.futex_waiters, 0);
    atomic_init(&packed_hdr.write_ticket, 10);
    packed_hdr.capacity_mask = 3;
    atomic_init(&packed_hdr.active_producers, 1);

    rbipc_slot_t packed_slot;
    atomic_init(&packed_slot.sequence, rbipc_slot_pack(0, 0, RBIPC_SLOT_EMPTY));
    atomic_init(&packed_slot.state, RBIPC_SLOT_RESERVED);
    atomic_init(&packed_slot.producer_pid, 9999999);
    atomic_init(&packed_slot.consumer_pid, 0);
    atomic_init(&packed_slot.acquire_timestamp_ns, 0);

    bool p_recovered = rbipc_audit_recover_dead_producer(&packed_hdr, &packed_slot, 0);
    assert(p_recovered == true);
    uint64_t p_seq = atomic_load_explicit(&packed_slot.sequence, memory_order_relaxed);
    assert(rbipc_slot_unpack_seq(p_seq) == 1);
    assert(rbipc_slot_unpack_state(p_seq) == RBIPC_SLOT_POISONED);

    /* 6. Dead consumer recovery on packed ring */
    atomic_init(&packed_slot.consumer_pid, 9999999);
    atomic_init(&packed_slot.acquire_timestamp_ns, 100);
    bool p_crecovered = rbipc_audit_recover_dead_consumer(&packed_hdr, &packed_slot, 4);
    assert(p_crecovered == true);
    uint64_t c_seq = atomic_load_explicit(&packed_slot.sequence, memory_order_relaxed);
    assert(rbipc_slot_unpack_seq(c_seq) == 4);
    assert(rbipc_slot_unpack_state(c_seq) == RBIPC_SLOT_EMPTY);

    /* 7. Ticket hole healing on packed ring */
    rbipc_sync_state_t packed_sync_state;
    rbipc_sync_state_init(&packed_sync_state, 1000000000ULL);
    atomic_init(&packed_slot.sequence, rbipc_slot_pack(5, 0, RBIPC_SLOT_EMPTY));
    atomic_init(&packed_slot.state, RBIPC_SLOT_EMPTY);
    atomic_init(&packed_hdr.write_ticket, 10);
    atomic_init(&packed_hdr.active_producers, 0);

    bool p_healed = rbipc_audit_check_and_heal_ticket_hole(&packed_hdr, &packed_slot, 5, &packed_sync_state);
    assert(p_healed == true);
    uint64_t h_seq = atomic_load_explicit(&packed_slot.sequence, memory_order_relaxed);
    assert(rbipc_slot_unpack_seq(h_seq) == 6);
    assert(rbipc_slot_unpack_state(h_seq) == RBIPC_SLOT_POISONED);

    printf("[test_unit_audit] All tests passed successfully.\n");
    return 0;
}
