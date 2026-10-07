/**
 * @file rbipc_predicate.h
 * @brief Domain predicates pattern: Pure, composable invariants and state inspection
 *
 * Implements the Predicates Pattern to isolate state queries, invariant contracts,
 * and boundary conditions into pure, highly readable, single-responsibility functions.
 */

#ifndef RBIPC_PREDICATE_H
#define RBIPC_PREDICATE_H

#include "rbipc.h"
#include "rbipc_attr.h"
#include "rbipc_math.h"
#include "rbipc_sync.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ============================================================================
 * Pointer & Descriptor Validation Predicates
 * ============================================================================ */

/**
 * @brief Predicate: Check if a pointer is non-null.
 */
RBIPC_INLINE RBIPC_CONST bool rbipc_is_valid_ptr(const void *ptr) {
    return ptr != NULL;
}

/**
 * @brief Predicate: Check if a pointer is null.
 */
RBIPC_INLINE RBIPC_CONST bool rbipc_is_null(const void *ptr) {
    return ptr == NULL;
}

/**
 * @brief Predicate: Check if a file descriptor is open/valid.
 */
RBIPC_INLINE RBIPC_CONST bool rbipc_is_valid_fd(int fd) {
    return fd >= 0;
}

/**
 * @brief Predicate: Check if a POSIX shared memory name is valid.
 * A valid POSIX SHM name begins with '/' and does not exceed 255 characters.
 */
RBIPC_INLINE RBIPC_PURE bool rbipc_is_valid_shm_name(const char *name) {
    if (!name || name[0] != '/') {
        return false;
    }
    return strlen(name) <= 255;
}

/* ============================================================================
 * Arithmetic, Geometry, and Capacity Predicates
 * ============================================================================ */

/**
 * @brief Predicate: Check if an integer is strictly a power of two.
 */
RBIPC_INLINE RBIPC_CONST bool rbipc_is_power_of_two(uint32_t val) {
    return (val > 0) && ((val & (val - 1)) == 0);
}

/**
 * @brief Predicate: Check if ring capacity is valid (power of two, >= 2, <= 2^30).
 */
RBIPC_INLINE RBIPC_CONST bool rbipc_is_valid_capacity(size_t capacity) {
    return (capacity >= 2) && (capacity <= 0x40000000UL) && ((capacity & (capacity - 1)) == 0);
}

/**
 * @brief Predicate: Check if slot size is within valid bounds.
 */
RBIPC_INLINE RBIPC_CONST bool rbipc_is_valid_slot_size(uint32_t slot_size) {
    return (slot_size > 0) && (slot_size <= (UINT32_MAX - RBIPC_CACHE_LINE));
}

/**
 * @brief Predicate: Check if page size is valid.
 */
RBIPC_INLINE RBIPC_CONST bool rbipc_is_valid_page_size(size_t page_size) {
    return (page_size > 0) && ((page_size & (page_size - 1)) == 0);
}

/* ============================================================================
 * Sequence Arithmetic Predicates
 * ============================================================================ */

/**
 * @brief Predicate: Check if two sequence numbers are equal (modulo 2^32).
 */
RBIPC_INLINE RBIPC_CONST bool rbipc_seq_is_equal(uint32_t a, uint32_t b) {
    return rbipc_seq_diff(a, b) == 0;
}

/**
 * @brief Predicate: Check if sequence number 'a' is ahead of 'b' (modulo 2^32).
 */
RBIPC_INLINE RBIPC_CONST bool rbipc_seq_is_ahead(uint32_t a, uint32_t b) {
    return rbipc_seq_diff(a, b) > 0;
}

/**
 * @brief Predicate: Check if sequence number 'a' is behind 'b' (modulo 2^32).
 */
RBIPC_INLINE RBIPC_CONST bool rbipc_seq_is_behind(uint32_t a, uint32_t b) {
    return rbipc_seq_diff(a, b) < 0;
}

/* ============================================================================
 * Slot State Machine Predicates
 * ============================================================================ */

/**
 * @brief Predicate: Check if a slot sequence indicates it is vacant for ticket 't'.
 */
RBIPC_INLINE RBIPC_CONST bool rbipc_slot_is_vacant(uint32_t seq, uint32_t ticket) {
    return rbipc_seq_diff(seq, ticket) == 0;
}

/**
 * @brief Predicate: Check if a slot sequence indicates it is ready for read ticket 't'.
 */
RBIPC_INLINE RBIPC_CONST bool rbipc_slot_is_ready(uint32_t seq, uint32_t ticket) {
    return rbipc_seq_diff(seq, ticket + 1) == 0;
}

/**
 * @brief Predicate: Check if slot state is EMPTY.
 */
RBIPC_INLINE RBIPC_CONST bool rbipc_slot_state_is_empty(uint32_t state) {
    return state == RBIPC_SLOT_EMPTY;
}

/**
 * @brief Predicate: Check if slot state is RESERVED.
 */
RBIPC_INLINE RBIPC_CONST bool rbipc_slot_state_is_reserved(uint32_t state) {
    return state == RBIPC_SLOT_RESERVED;
}

/**
 * @brief Predicate: Check if slot state is COMMITTED.
 */
RBIPC_INLINE RBIPC_CONST bool rbipc_slot_state_is_committed(uint32_t state) {
    return state == RBIPC_SLOT_COMMITTED;
}

/**
 * @brief Predicate: Check if slot state is POISONED.
 */
RBIPC_INLINE RBIPC_CONST bool rbipc_slot_state_is_poisoned(uint32_t state) {
    return state == RBIPC_SLOT_POISONED;
}

/* ============================================================================
 * Header & Ring Invariant Predicates
 * ============================================================================ */

/**
 * @brief Predicate: Validate header magic, version, and core configuration invariants.
 */
RBIPC_INLINE RBIPC_PURE bool rbipc_header_is_valid(const rbipc_shm_header_t *hdr) {
    if (!hdr) return false;
    if (hdr->magic != RBIPC_MAGIC || hdr->version != RBIPC_VERSION) return false;
    if (!rbipc_is_power_of_two(hdr->capacity)) return false;
    if (hdr->capacity < 2 || hdr->slot_size == 0 || hdr->data_size == 0) return false;
    if (hdr->data_offset == 0 || hdr->total_shm_size <= hdr->data_offset) return false;
    return true;
}

/**
 * @brief Predicate: Check if the ring buffer has signaled shutdown.
 */
RBIPC_INLINE bool rbipc_ring_is_shutdown(const rbipc_shm_header_t *hdr) {
    if (RBIPC_UNLIKELY(!hdr)) return true;
    return atomic_load_explicit(&hdr->shutdown_flag, memory_order_relaxed) != 0;
}

/**
 * @brief Predicate: Check if shutdown is signaled and all written items are drained.
 */
RBIPC_INLINE bool rbipc_ring_is_drained_on_shutdown(const rbipc_shm_header_t *hdr, uint32_t read_ticket) {
    if (RBIPC_UNLIKELY(rbipc_ring_is_shutdown(hdr))) {
        uint32_t cur_write = atomic_load_explicit(&hdr->write_ticket, memory_order_relaxed);
        return read_ticket >= cur_write;
    }
    return false;
}

/* ============================================================================
 * Synchronization & Waiter Predicates
 * ============================================================================ */

/**
 * @brief Predicate: Check if a sync state has a timeout deadline.
 */
RBIPC_INLINE RBIPC_CONST bool rbipc_sync_has_deadline(const rbipc_sync_state_t *state) {
    return state != NULL && state->has_deadline;
}

/**
 * @brief Predicate: Check if a sync state deadline has expired.
 */
RBIPC_INLINE RBIPC_PURE bool rbipc_sync_is_expired(const rbipc_sync_state_t *state, uint64_t now_ns) {
    if (!state || !state->has_deadline) return false;
    return now_ns >= state->deadline_ns;
}

/**
 * @brief Predicate: Check if any threads are sleeping on a futex word.
 */
RBIPC_INLINE bool rbipc_sync_has_waiters(const _Atomic uint32_t *waiters) {
    return waiters != NULL && atomic_load_explicit(waiters, memory_order_seq_cst) > 0;
}

/**
 * @brief Predicate: Check if a wake syscall should be triggered.
 * Returns true if waiters pointer is NULL (safe default) or if waiters > 0.
 */
RBIPC_INLINE bool rbipc_sync_should_wake(const _Atomic uint32_t *waiters) {
    return !waiters || atomic_load_explicit(waiters, memory_order_seq_cst) > 0;
}

#endif /* RBIPC_PREDICATE_H */
