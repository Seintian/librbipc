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
 * @brief Evaluate whether a given memory pointer is non-null.
 *
 * @param[in] ptr Pointer address to inspect.
 * @return true if @p ptr is non-null, false otherwise.
 */
RBIPC_INLINE RBIPC_CONST bool rbipc_is_valid_ptr(const void *ptr) {
    return ptr != NULL;
}

/**
 * @brief Evaluate whether a given memory pointer is null.
 *
 * @param[in] ptr Pointer address to inspect.
 * @return true if @p ptr is NULL, false otherwise.
 */
RBIPC_INLINE RBIPC_CONST bool rbipc_is_null(const void *ptr) {
    return ptr == NULL;
}

/**
 * @brief Evaluate whether a file descriptor represents a non-negative open descriptor.
 *
 * @param[in] fd File descriptor integer.
 * @return true if @p fd >= 0, false if negative.
 */
RBIPC_INLINE RBIPC_CONST bool rbipc_is_valid_fd(int fd) {
    return fd >= 0;
}

/**
 * @brief Evaluate whether a string is a valid POSIX shared memory name.
 *
 * @details A compliant POSIX shared memory name must begin with a forward slash ('/'),
 * contain no additional slashes, and not exceed NAME_MAX (255 characters).
 *
 * @param[in] name NUL-terminated shared memory name string.
 * @return true if valid, false otherwise.
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
 * @brief Evaluate whether an unsigned 32-bit integer is strictly a non-zero power of two.
 *
 * @param[in] val Integer value to test.
 * @return true if @p val is a power of two, false otherwise.
 */
RBIPC_INLINE RBIPC_CONST bool rbipc_is_power_of_two(uint32_t val) {
    return (val > 0) && ((val & (val - 1)) == 0);
}

/**
 * @brief Evaluate whether a ring buffer capacity satisfies architectural constraints.
 *
 * @details Valid capacity must be a power of two, at least 2 slots, and at most \f$2^{30}\f$ slots.
 *
 * @param[in] capacity Slot capacity count to validate.
 * @return true if within admissible operational bounds, false otherwise.
 */
RBIPC_INLINE RBIPC_CONST bool rbipc_is_valid_capacity(size_t capacity) {
    return (capacity >= 2) && (capacity <= 0x40000000UL) && ((capacity & (capacity - 1)) == 0);
}

/**
 * @brief Evaluate whether a per-slot payload size is within valid limits.
 *
 * @param[in] slot_size Maximum payload capacity in bytes.
 * @return true if @p slot_size > 0 and does not risk integer overflow, false otherwise.
 */
RBIPC_INLINE RBIPC_CONST bool rbipc_is_valid_slot_size(uint32_t slot_size) {
    return (slot_size > 0) && (slot_size <= (UINT32_MAX - RBIPC_CACHE_LINE));
}

/**
 * @brief Evaluate whether system virtual memory page size is a valid power-of-two.
 *
 * @param[in] page_size Memory page size in bytes (typically 4096).
 * @return true if non-zero power of two, false otherwise.
 */
RBIPC_INLINE RBIPC_CONST bool rbipc_is_valid_page_size(size_t page_size) {
    return (page_size > 0) && ((page_size & (page_size - 1)) == 0);
}

/* ============================================================================
 * Sequence Arithmetic Predicates
 * ============================================================================ */

/**
 * @brief Evaluate whether two monotonic sequence numbers are equal under modulo-2^32 arithmetic.
 *
 * @param[in] a Sequence number A.
 * @param[in] b Sequence number B.
 * @return true if sequence numbers are identical, false otherwise.
 */
RBIPC_INLINE RBIPC_CONST bool rbipc_seq_is_equal(uint32_t a, uint32_t b) {
    return rbipc_seq_diff(a, b) == 0;
}

/**
 * @brief Evaluate whether sequence number @p a is strictly ahead of sequence number @p b.
 *
 * @param[in] a Sequence number A.
 * @param[in] b Sequence number B.
 * @return true if @p a is ahead of @p b, false otherwise.
 */
RBIPC_INLINE RBIPC_CONST bool rbipc_seq_is_ahead(uint32_t a, uint32_t b) {
    return rbipc_seq_diff(a, b) > 0;
}

/**
 * @brief Evaluate whether sequence number @p a is strictly behind sequence number @p b.
 *
 * @param[in] a Sequence number A.
 * @param[in] b Sequence number B.
 * @return true if @p a is behind @p b, false otherwise.
 */
RBIPC_INLINE RBIPC_CONST bool rbipc_seq_is_behind(uint32_t a, uint32_t b) {
    return rbipc_seq_diff(a, b) < 0;
}

/* ============================================================================
 * Slot State Machine Predicates
 * ============================================================================ */

/**
 * @brief Evaluate whether a slot descriptor sequence matches vacant state for ticket @p ticket.
 *
 * @param[in] seq    Current sequence word loaded from slot descriptor.
 * @param[in] ticket Reserving producer ticket number.
 * @return true if slot is ready for producer reservation, false otherwise.
 */
RBIPC_INLINE RBIPC_CONST bool rbipc_slot_is_vacant(uint32_t seq, uint32_t ticket) {
    return rbipc_seq_diff(seq, ticket) == 0;
}

/**
 * @brief Evaluate whether a slot descriptor sequence matches ready state for ticket @p ticket.
 *
 * @param[in] seq    Current sequence word loaded from slot descriptor.
 * @param[in] ticket Acquiring consumer ticket number.
 * @return true if slot payload has been committed by producer, false otherwise.
 */
RBIPC_INLINE RBIPC_CONST bool rbipc_slot_is_ready(uint32_t seq, uint32_t ticket) {
    return rbipc_seq_diff(seq, ticket + 1) == 0;
}

/**
 * @brief Evaluate whether slot state word equals RBIPC_SLOT_EMPTY.
 *
 * @param[in] state Slot state word.
 * @return true if empty, false otherwise.
 */
RBIPC_INLINE RBIPC_CONST bool rbipc_slot_state_is_empty(uint32_t state) {
    return state == RBIPC_SLOT_EMPTY;
}

/**
 * @brief Evaluate whether slot state word equals RBIPC_SLOT_RESERVED.
 *
 * @param[in] state Slot state word.
 * @return true if reserved, false otherwise.
 */
RBIPC_INLINE RBIPC_CONST bool rbipc_slot_state_is_reserved(uint32_t state) {
    return state == RBIPC_SLOT_RESERVED;
}

/**
 * @brief Evaluate whether slot state word equals RBIPC_SLOT_COMMITTED.
 *
 * @param[in] state Slot state word.
 * @return true if committed, false otherwise.
 */
RBIPC_INLINE RBIPC_CONST bool rbipc_slot_state_is_committed(uint32_t state) {
    return state == RBIPC_SLOT_COMMITTED;
}

/**
 * @brief Evaluate whether slot state word equals RBIPC_SLOT_POISONED.
 *
 * @param[in] state Slot state word.
 * @return true if poisoned, false otherwise.
 */
RBIPC_INLINE RBIPC_CONST bool rbipc_slot_state_is_poisoned(uint32_t state) {
    return state == RBIPC_SLOT_POISONED;
}

/* ============================================================================
 * Header & Ring Invariant Predicates
 * ============================================================================ */

/**
 * @brief Validate all structural control header invariants and geometric parameters.
 *
 * @param[in] hdr Shared memory control header.
 * @return true if header matches magic signature, version, and power-of-two constraints; false otherwise.
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
 * @brief Evaluate whether the ring buffer has entered cooperative shutdown state.
 *
 * @param[in] hdr Shared memory control header.
 * @return true if shutdown flag is set, false otherwise.
 */
RBIPC_INLINE bool rbipc_ring_is_shutdown(const rbipc_shm_header_t *hdr) {
    if (RBIPC_UNLIKELY(!hdr)) return true;
    return atomic_load_explicit(&hdr->shutdown_flag, memory_order_relaxed) != 0;
}

/**
 * @brief Evaluate whether the ring buffer is shut down and all committed data has been drained.
 *
 * @param[in] hdr         Shared memory control header.
 * @param[in] read_ticket Current reader ticket being processed.
 * @return true if shutdown is signaled and all published messages are consumed, false otherwise.
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
 * @brief Evaluate whether a synchronization state has an active deadline.
 *
 * @param[in] state Pointer to sync state.
 * @return true if deadline is configured, false otherwise.
 */
RBIPC_INLINE RBIPC_CONST bool rbipc_sync_has_deadline(const rbipc_sync_state_t *state) {
    return state != NULL && state->has_deadline;
}

/**
 * @brief Evaluate whether the synchronization state deadline has expired relative to current time.
 *
 * @param[in] state  Pointer to sync state.
 * @param[in] now_ns Current monotonic timestamp in nanoseconds.
 * @return true if expired, false otherwise.
 */
RBIPC_INLINE RBIPC_PURE bool rbipc_sync_is_expired(const rbipc_sync_state_t *state, uint64_t now_ns) {
    if (!state || !state->has_deadline) return false;
    return now_ns >= state->deadline_ns;
}

/**
 * @brief Evaluate whether any threads are actively suspended waiting on a futex word.
 *
 * @param[in] waiters Atomic counter of active futex waiters.
 * @return true if waiter count > 0, false otherwise.
 */
RBIPC_INLINE bool rbipc_sync_has_waiters(const _Atomic uint32_t *waiters) {
    return waiters != NULL && atomic_load_explicit(waiters, memory_order_seq_cst) > 0;
}

/**
 * @brief Determine whether a futex wakeup kernel syscall should be dispatched.
 *
 * @param[in] waiters Atomic counter of active futex waiters (may be NULL).
 * @return true if @p waiters is NULL (defensive fallback) or waiter count > 0; false if 0 waiters.
 */
RBIPC_INLINE bool rbipc_sync_should_wake(const _Atomic uint32_t *waiters) {
    return !waiters || atomic_load_explicit(waiters, memory_order_seq_cst) > 0;
}

#endif /* RBIPC_PREDICATE_H */
