/**
 * @file test_unit_predicates.c
 * @brief Unit tests for domain predicates pattern and invariant assertions
 */

#include "rbipc.h"
#include "../src/rbipc_predicate.h"
#include "../src/rbipc_sync.h"

#include <stdio.h>
#include <assert.h>
#include <limits.h>

static void test_pointer_and_descriptor_predicates(void) {
    int dummy = 42;
    assert(rbipc_is_valid_ptr(&dummy) == true);
    assert(rbipc_is_valid_ptr(NULL) == false);

    assert(rbipc_is_null(NULL) == true);
    assert(rbipc_is_null(&dummy) == false);

    assert(rbipc_is_valid_fd(0) == true);
    assert(rbipc_is_valid_fd(3) == true);
    assert(rbipc_is_valid_fd(-1) == false);
    assert(rbipc_is_valid_fd(-99) == false);

    assert(rbipc_is_valid_shm_name("/my_ring") == true);
    assert(rbipc_is_valid_shm_name("/") == false);               /* Empty identifier after leading slash rejected */
    assert(rbipc_is_valid_shm_name("/nested/ring") == false);     /* Nested slashes prohibited by POSIX standard */
    assert(rbipc_is_valid_shm_name("/dev/shm/ring") == false);    /* Filesystem path with multiple slashes prohibited */
    assert(rbipc_is_valid_shm_name("my_ring") == false);          /* Missing leading slash */
    assert(rbipc_is_valid_shm_name(NULL) == false);
}

static void test_arithmetic_and_geometry_predicates(void) {
    assert(rbipc_is_power_of_two(0) == false);
    assert(rbipc_is_power_of_two(1) == true);
    assert(rbipc_is_power_of_two(2) == true);
    assert(rbipc_is_power_of_two(3) == false);
    assert(rbipc_is_power_of_two(4) == true);
    assert(rbipc_is_power_of_two(1024) == true);
    assert(rbipc_is_power_of_two(1025) == false);

    assert(rbipc_is_valid_capacity(0) == false);
    assert(rbipc_is_valid_capacity(1) == false); /* capacity must be >= 2 */
    assert(rbipc_is_valid_capacity(2) == true);
    assert(rbipc_is_valid_capacity(1024) == true);
    assert(rbipc_is_valid_capacity(1023) == false);

    assert(rbipc_is_valid_slot_size(0) == false);
    assert(rbipc_is_valid_slot_size(64) == true);
    assert(rbipc_is_valid_slot_size(UINT32_MAX) == false);

    assert(rbipc_is_valid_page_size(0) == false);
    assert(rbipc_is_valid_page_size(4096) == true);
    assert(rbipc_is_valid_page_size(65536) == true);
    assert(rbipc_is_valid_page_size(4000) == false);
}

static void test_sequence_and_slot_predicates(void) {
    assert(rbipc_seq_is_equal(100, 100) == true);
    assert(rbipc_seq_is_equal(100, 101) == false);
    assert(rbipc_seq_is_ahead(101, 100) == true);
    assert(rbipc_seq_is_ahead(100, 101) == false);
    assert(rbipc_seq_is_behind(100, 101) == true);
    assert(rbipc_seq_is_behind(101, 100) == false);

    /* Slot vacancy: sequence == ticket */
    assert(rbipc_slot_is_vacant(0, 0) == true);
    assert(rbipc_slot_is_vacant(10, 10) == true);
    assert(rbipc_slot_is_vacant(9, 10) == false);

    /* Slot readiness: sequence == ticket + 1 */
    assert(rbipc_slot_is_ready(1, 0) == true);
    assert(rbipc_slot_is_ready(11, 10) == true);
    assert(rbipc_slot_is_ready(10, 10) == false);

    /* Slot states */
    assert(rbipc_slot_state_is_empty(RBIPC_SLOT_EMPTY) == true);
    assert(rbipc_slot_state_is_empty(RBIPC_SLOT_RESERVED) == false);
    assert(rbipc_slot_state_is_reserved(RBIPC_SLOT_RESERVED) == true);
    assert(rbipc_slot_state_is_committed(RBIPC_SLOT_COMMITTED) == true);
    assert(rbipc_slot_state_is_poisoned(RBIPC_SLOT_POISONED) == true);
}

static void test_header_and_sync_predicates(void) {
    rbipc_shm_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    assert(rbipc_header_is_valid(&hdr) == false);

    hdr.magic = RBIPC_MAGIC;
    hdr.version = RBIPC_VERSION;
    hdr.capacity = 1024;
    hdr.slot_size = 256;
    hdr.data_offset = 4096;
    hdr.data_size = 262144;
    hdr.total_shm_size = hdr.data_offset + hdr.data_size;
    assert(rbipc_header_is_valid(&hdr) == true);

    atomic_init(&hdr.shutdown_flag, 0);
    assert(rbipc_ring_is_shutdown(&hdr) == false);
    atomic_store(&hdr.shutdown_flag, 1);
    assert(rbipc_ring_is_shutdown(&hdr) == true);

    /* Shutdown drain check */
    atomic_init(&hdr.write_ticket, 50);
    assert(rbipc_ring_is_drained_on_shutdown(&hdr, 49) == false);
    assert(rbipc_ring_is_drained_on_shutdown(&hdr, 50) == true);
    assert(rbipc_ring_is_drained_on_shutdown(&hdr, 51) == true);

    /* Sync predicates */
    rbipc_sync_state_t sync_state;
    rbipc_sync_state_init(&sync_state, UINT64_MAX);
    assert(rbipc_sync_has_deadline(&sync_state) == false);
    assert(rbipc_sync_is_expired(&sync_state, 1000000000ULL) == false);

    rbipc_sync_state_init(&sync_state, 100);
    assert(rbipc_sync_has_deadline(&sync_state) == true);
    assert(rbipc_sync_is_expired(&sync_state, sync_state.deadline_ns - 1) == false);
    assert(rbipc_sync_is_expired(&sync_state, sync_state.deadline_ns) == true);

    _Atomic uint32_t waiters;
    atomic_init(&waiters, 0);
    assert(rbipc_sync_has_waiters(&waiters) == false);
    assert(rbipc_sync_should_wake(&waiters) == false);
    assert(rbipc_sync_should_wake(NULL) == true); /* safe default */

    atomic_store(&waiters, 1);
    assert(rbipc_sync_has_waiters(&waiters) == true);
    assert(rbipc_sync_should_wake(&waiters) == true);
}

int main(void) {
    printf("[test_unit_predicates] Running tests...\n");
    test_pointer_and_descriptor_predicates();
    test_arithmetic_and_geometry_predicates();
    test_sequence_and_slot_predicates();
    test_header_and_sync_predicates();
    printf("[test_unit_predicates] All tests passed!\n");
    return 0;
}
