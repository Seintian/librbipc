/**
 * @file rbipc_io.c
 * @brief Zero-copy input/output queueing operations, ticket coordination, and batching
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "rbipc.h"
#include "rbipc_internal.h"
#include "rbipc_io.h"
#include "rbipc_math.h"
#include "rbipc_slot.h"
#include "rbipc_sync.h"
#include "rbipc_attr.h"
#include "rbipc_predicate.h"

#include <unistd.h>
#include <errno.h>

/* ============================================================================
 * Atomic Synchronization and State Transition Subroutines
 * ============================================================================ */

/**
 * @brief Atomic procedure: Attempt compare-and-swap reservation of producer ticket range.
 */
RBIPC_INLINE bool rbipc_io_try_claim_write_ticket(rbipc_shm_header_t * RBIPC_RESTRICT hdr,
                                                             uint32_t * RBIPC_RESTRICT t,
                                                             uint32_t count) {
    return atomic_compare_exchange_weak_explicit(&hdr->write_ticket, t, *t + count,
                                                 memory_order_relaxed, memory_order_relaxed);
}

/**
 * @brief Atomic procedure: Attempt compare-and-swap reservation of consumer ticket range.
 */
RBIPC_INLINE bool rbipc_io_try_claim_read_ticket(rbipc_shm_header_t * RBIPC_RESTRICT hdr,
                                                            uint32_t * RBIPC_RESTRICT t,
                                                            uint32_t count) {
    return atomic_compare_exchange_weak_explicit(&hdr->read_ticket, t, *t + count,
                                                 memory_order_relaxed, memory_order_relaxed);
}

/**
 * @brief Atomic procedure: Audit slot descriptor and recover if holding producer terminated prematurely.
 */
static void rbipc_io_audit_and_recover_dead_peer(rbipc_shm_header_t *hdr, rbipc_slot_t *slot, uint32_t ticket) {
    uint32_t state = atomic_load_explicit(&slot->state, memory_order_acquire);
    if (rbipc_slot_state_is_reserved(state)) {
        uint32_t pid = atomic_load_explicit(&slot->producer_pid, memory_order_relaxed);
        if (pid > 0 && !rbipc_slot_is_peer_alive((pid_t)pid)) {
            if (rbipc_slot_poison(slot, ticket)) {
                rbipc_sync_wake_one(&hdr->futex_seq, &hdr->futex_waiters);
            }
        }
    }
}

/**
 * @brief Atomic procedure: Finalize slot write reservation and prime memory caches.
 */
RBIPC_INLINE void rbipc_io_complete_write_reservation(rbipc_ring_t * RBIPC_RESTRICT ring,
                                                                 rbipc_slot_t *slot, uint32_t ticket,
                                                                 void ** RBIPC_RESTRICT out_buf,
                                                                 uint32_t * RBIPC_RESTRICT out_ticket) {
    rbipc_slot_mark_reserved(slot, ring->cached_pid);
    *out_buf = rbipc_io_calc_slot_ptr(ring, ticket);
    *out_ticket = ticket;
    rbipc_io_prefetch_write_next(ring, ticket + 1);
}

/**
 * @brief Atomic procedure: Evaluate buffer full condition and execute passive futex backoff.
 */
static int rbipc_io_handle_write_backoff(rbipc_sync_state_t * RBIPC_RESTRICT sync_state,
                                                   rbipc_shm_header_t *hdr,
                                                   uint64_t timeout_ns,
                                                   uint32_t * RBIPC_RESTRICT t) {
    if (timeout_ns == 0) {
        return RBIPC_ERR_FULL;
    }
    int rc = rbipc_sync_backoff(sync_state, &hdr->write_futex_seq, &hdr->write_waiters);
    if (RBIPC_UNLIKELY(rc != 0)) {
        return rc;
    }
    *t = atomic_load_explicit(&hdr->write_ticket, memory_order_relaxed);
    return RBIPC_OK;
}

/**
 * @brief Atomic procedure: Finalize committed read acquisition and extract message metadata.
 */
RBIPC_INLINE int rbipc_io_complete_read_acquisition(const rbipc_ring_t * RBIPC_RESTRICT ring,
                                                               const rbipc_slot_t *slot, uint32_t ticket,
                                                               const void ** RBIPC_RESTRICT out_buf,
                                                               uint32_t * RBIPC_RESTRICT out_len,
                                                               uint32_t * RBIPC_RESTRICT out_ticket) {
    uint32_t state = atomic_load_explicit(&slot->state, memory_order_acquire);
    if (RBIPC_LIKELY(rbipc_slot_state_is_committed(state))) {
        *out_buf = rbipc_io_calc_slot_ptr(ring, ticket);
        *out_len = atomic_load_explicit(&slot->len, memory_order_acquire);
        *out_ticket = ticket;
        rbipc_io_prefetch_read_next(ring, ticket + 1);
        return RBIPC_OK;
    }
    if (rbipc_slot_state_is_poisoned(state)) {
        *out_buf = NULL;
        *out_len = 0;
        *out_ticket = ticket;
        return RBIPC_ERR_POISONED;
    }
    return RBIPC_ERR_BUSY;
}

/**
 * @brief Atomic procedure: Evaluate buffer empty condition and execute passive futex backoff.
 */
static int rbipc_io_handle_read_backoff(rbipc_sync_state_t * RBIPC_RESTRICT sync_state,
                                                   rbipc_shm_header_t *hdr,
                                                   rbipc_slot_t *slot,
                                                   uint32_t t,
                                                   uint64_t timeout_ns,
                                                   uint32_t * RBIPC_RESTRICT out_next_t) {
    rbipc_io_audit_and_recover_dead_peer(hdr, slot, t);

    if (RBIPC_UNLIKELY(rbipc_ring_is_drained_on_shutdown(hdr, t))) {
        return RBIPC_ERR_SHUTDOWN;
    }

    if (timeout_ns == 0) {
        return RBIPC_ERR_EMPTY;
    }

    int rc = rbipc_sync_backoff(sync_state, &hdr->futex_seq, &hdr->futex_waiters);
    if (RBIPC_UNLIKELY(rc != 0)) {
        return rc;
    }

    *out_next_t = atomic_load_explicit(&hdr->read_ticket, memory_order_relaxed);
    return RBIPC_OK;
}

/* ============================================================================
 * Public Single-Item Write Interface
 * ============================================================================ */

int rbipc_reserve_write_timeout(rbipc_ring_t * RBIPC_RESTRICT ring, uint32_t len, uint64_t timeout_ns,
                                void ** RBIPC_RESTRICT out_buf, uint32_t * RBIPC_RESTRICT ticket) {
    if (RBIPC_UNLIKELY(!ring || !ring->hdr || !out_buf || !ticket || len > ring->hdr->slot_size)) {
        return RBIPC_ERR_INVAL;
    }

    rbipc_shm_header_t *hdr = ring->hdr;
    const uint32_t mask = hdr->capacity_mask;
    uint32_t t = atomic_load_explicit(&hdr->write_ticket, memory_order_relaxed);
    rbipc_slot_t *slots = (rbipc_slot_t *)RBIPC_ASSUME_ALIGNED(ring->slots, RBIPC_CACHE_LINE);

    rbipc_sync_state_t sync_state;
    rbipc_sync_state_init(&sync_state, timeout_ns);

    for (;;) {
        if (RBIPC_UNLIKELY(rbipc_ring_is_shutdown(hdr))) {
            return RBIPC_ERR_SHUTDOWN;
        }

        rbipc_slot_t *slot = &slots[t & mask];
        uint32_t seq = atomic_load_explicit(&slot->sequence, memory_order_acquire);

        if (RBIPC_LIKELY(rbipc_slot_is_vacant(seq, t))) {
            if (RBIPC_LIKELY(rbipc_io_try_claim_write_ticket(hdr, &t, 1))) {
                rbipc_io_complete_write_reservation(ring, slot, t, out_buf, ticket);
                return RBIPC_OK;
            }
            sync_state.spin_count = 0;
        } else if (rbipc_seq_is_behind(seq, t)) {
            int rc = rbipc_io_handle_write_backoff(&sync_state, hdr, timeout_ns, &t);
            if (RBIPC_UNLIKELY(rc != RBIPC_OK)) {
                return rc;
            }
        } else {
            t = atomic_load_explicit(&hdr->write_ticket, memory_order_relaxed);
            sync_state.spin_count = 0;
        }
    }
}

int rbipc_reserve_write(rbipc_ring_t * RBIPC_RESTRICT ring, uint32_t len,
                        void ** RBIPC_RESTRICT out_buf, uint32_t * RBIPC_RESTRICT ticket) {
    return rbipc_reserve_write_timeout(ring, len, UINT64_MAX, out_buf, ticket);
}

int rbipc_reserve_write_nonblock(rbipc_ring_t * RBIPC_RESTRICT ring, uint32_t len,
                                 void ** RBIPC_RESTRICT out_buf, uint32_t * RBIPC_RESTRICT ticket) {
    return rbipc_reserve_write_timeout(ring, len, 0, out_buf, ticket);
}

int rbipc_commit_write(rbipc_ring_t * RBIPC_RESTRICT ring, uint32_t ticket, uint32_t written_len) {
    if (RBIPC_UNLIKELY(!ring || !ring->hdr || written_len > ring->hdr->slot_size)) {
        return RBIPC_ERR_INVAL;
    }

    rbipc_shm_header_t *hdr = ring->hdr;
    rbipc_slot_t *slots = (rbipc_slot_t *)RBIPC_ASSUME_ALIGNED(ring->slots, RBIPC_CACHE_LINE);
    rbipc_slot_t *slot = &slots[ticket & hdr->capacity_mask];

    rbipc_slot_commit(slot, ticket, written_len);
    if (RBIPC_UNLIKELY(rbipc_sync_has_waiters(&hdr->futex_waiters))) {
        rbipc_sync_wake_one(&hdr->futex_seq, &hdr->futex_waiters);
    }

    return RBIPC_OK;
}

int rbipc_abort_write(rbipc_ring_t * RBIPC_RESTRICT ring, uint32_t ticket) {
    if (RBIPC_UNLIKELY(!ring || !ring->hdr)) {
        return RBIPC_ERR_INVAL;
    }

    rbipc_shm_header_t *hdr = ring->hdr;
    rbipc_slot_t *slots = (rbipc_slot_t *)RBIPC_ASSUME_ALIGNED(ring->slots, RBIPC_CACHE_LINE);
    rbipc_slot_t *slot = &slots[ticket & hdr->capacity_mask];

    if (rbipc_slot_poison(slot, ticket)) {
        rbipc_sync_wake_one(&hdr->futex_seq, &hdr->futex_waiters);
    }

    return RBIPC_OK;
}

/* ============================================================================
 * Public Single-Item Read Interface
 * ============================================================================ */

int rbipc_read_acquire_timeout(rbipc_ring_t * RBIPC_RESTRICT ring, uint64_t timeout_ns,
                               const void ** RBIPC_RESTRICT out_buf, uint32_t * RBIPC_RESTRICT out_len,
                               uint32_t * RBIPC_RESTRICT ticket) {
    if (RBIPC_UNLIKELY(!ring || !ring->hdr || !out_buf || !out_len || !ticket)) {
        return RBIPC_ERR_INVAL;
    }

    rbipc_shm_header_t *hdr = ring->hdr;
    const uint32_t mask = hdr->capacity_mask;
    uint32_t t = atomic_load_explicit(&hdr->read_ticket, memory_order_relaxed);
    rbipc_slot_t *slots = (rbipc_slot_t *)RBIPC_ASSUME_ALIGNED(ring->slots, RBIPC_CACHE_LINE);

    rbipc_sync_state_t sync_state;
    rbipc_sync_state_init(&sync_state, timeout_ns);

    for (;;) {
        rbipc_slot_t *slot = &slots[t & mask];
        uint32_t seq = atomic_load_explicit(&slot->sequence, memory_order_acquire);

        if (RBIPC_LIKELY(rbipc_slot_is_ready(seq, t))) {
            if (RBIPC_LIKELY(rbipc_io_try_claim_read_ticket(hdr, &t, 1))) {
                return rbipc_io_complete_read_acquisition(ring, slot, t, out_buf, out_len, ticket);
            }
            sync_state.spin_count = 0;
        } else if (rbipc_seq_is_behind(seq, t + 1)) {
            int rc = rbipc_io_handle_read_backoff(&sync_state, hdr, slot, t, timeout_ns, &t);
            if (RBIPC_UNLIKELY(rc != RBIPC_OK)) {
                return rc;
            }
        } else {
            t = atomic_load_explicit(&hdr->read_ticket, memory_order_relaxed);
            sync_state.spin_count = 0;
        }
    }
}

int rbipc_read_acquire(rbipc_ring_t * RBIPC_RESTRICT ring, const void ** RBIPC_RESTRICT out_buf,
                       uint32_t * RBIPC_RESTRICT out_len, uint32_t * RBIPC_RESTRICT ticket) {
    return rbipc_read_acquire_timeout(ring, UINT64_MAX, out_buf, out_len, ticket);
}

int rbipc_read_acquire_nonblock(rbipc_ring_t * RBIPC_RESTRICT ring, const void ** RBIPC_RESTRICT out_buf,
                                uint32_t * RBIPC_RESTRICT out_len, uint32_t * RBIPC_RESTRICT ticket) {
    return rbipc_read_acquire_timeout(ring, 0, out_buf, out_len, ticket);
}

int rbipc_read_release(rbipc_ring_t * RBIPC_RESTRICT ring, uint32_t ticket) {
    if (RBIPC_UNLIKELY(!ring || !ring->hdr)) {
        return RBIPC_ERR_INVAL;
    }

    rbipc_shm_header_t *hdr = ring->hdr;
    rbipc_slot_t *slots = (rbipc_slot_t *)RBIPC_ASSUME_ALIGNED(ring->slots, RBIPC_CACHE_LINE);
    rbipc_slot_t *slot = &slots[ticket & hdr->capacity_mask];

    rbipc_slot_release(slot, ticket, hdr->capacity);
    if (RBIPC_UNLIKELY(rbipc_sync_has_waiters(&hdr->write_waiters))) {
        rbipc_sync_wake_one(&hdr->write_futex_seq, &hdr->write_waiters);
    }

    return RBIPC_OK;
}

/* ============================================================================
 * Batch Queue (B-Queue) Vector Operations
 * ============================================================================ */

/**
 * @brief Atomic procedure: Scan contiguous vacant slots starting from ticket position.
 */
static uint32_t rbipc_io_scan_vacant_slots(const rbipc_ring_t *ring, uint32_t t, uint32_t count) {
    const uint32_t mask = ring->hdr->capacity_mask;
    const rbipc_slot_t *slots = (const rbipc_slot_t *)RBIPC_ASSUME_ALIGNED(ring->slots, RBIPC_CACHE_LINE);
    uint32_t avail = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const rbipc_slot_t *slot = &slots[(t + i) & mask];
        uint32_t seq = atomic_load_explicit(&slot->sequence, memory_order_acquire);
        if (rbipc_slot_is_vacant(seq, t + i)) {
            avail++;
        } else {
            break;
        }
    }
    return avail;
}

/**
 * @brief Atomic procedure: Populate write reservation vector descriptors.
 */
static void rbipc_io_populate_reserved_batch(rbipc_ring_t * RBIPC_RESTRICT ring,
                                             uint32_t t, uint32_t count,
                                             rbipc_iovec_t * RBIPC_RESTRICT iovecs) {
    const uint32_t mask = ring->hdr->capacity_mask;
    const uint32_t slot_size = ring->hdr->slot_size;
    pid_t my_pid = ring->cached_pid;
    rbipc_slot_t *slots = (rbipc_slot_t *)RBIPC_ASSUME_ALIGNED(ring->slots, RBIPC_CACHE_LINE);
    char *data_base = (char *)RBIPC_ASSUME_ALIGNED(ring->data_map, RBIPC_CACHE_LINE);

    for (uint32_t i = 0; i < count; ++i) {
        uint32_t ticket_i = t + i;
        rbipc_slot_t *slot_i = &slots[ticket_i & mask];
        rbipc_slot_mark_reserved(slot_i, my_pid);
        iovecs[i].buf = data_base + ((size_t)(ticket_i & mask) * slot_size);
        iovecs[i].ticket = ticket_i;
        iovecs[i].max_len = slot_size;
    }
    RBIPC_PREFETCH(&slots[(t + count) & mask], 1, 3);
}

int rbipc_reserve_write_batch(rbipc_ring_t * RBIPC_RESTRICT ring, uint32_t count,
                              rbipc_iovec_t * RBIPC_RESTRICT iovecs, uint32_t * RBIPC_RESTRICT out_reserved) {
    if (RBIPC_UNLIKELY(!ring || !ring->hdr || !iovecs || !out_reserved || count == 0)) {
        return RBIPC_ERR_INVAL;
    }

    rbipc_shm_header_t *hdr = ring->hdr;
    if (count > hdr->capacity) {
        count = hdr->capacity;
    }

    uint32_t t = atomic_load_explicit(&hdr->write_ticket, memory_order_relaxed);

    rbipc_sync_state_t sync_state;
    rbipc_sync_state_init(&sync_state, UINT64_MAX);

    for (;;) {
        if (RBIPC_UNLIKELY(rbipc_ring_is_shutdown(hdr))) {
            return RBIPC_ERR_SHUTDOWN;
        }

        uint32_t avail = rbipc_io_scan_vacant_slots(ring, t, count);
        if (avail > 0) {
            if (rbipc_io_try_claim_write_ticket(hdr, &t, avail)) {
                rbipc_io_populate_reserved_batch(ring, t, avail, iovecs);
                *out_reserved = avail;
                return RBIPC_OK;
            }
            sync_state.spin_count = 0;
        } else {
            int rc = rbipc_sync_backoff(&sync_state, &hdr->write_futex_seq, &hdr->write_waiters);
            if (RBIPC_UNLIKELY(rc != 0)) {
                return rc;
            }
            t = atomic_load_explicit(&hdr->write_ticket, memory_order_relaxed);
        }
    }
}

/**
 * @brief Atomic procedure: Validate batch payload lengths against slot limit.
 */
static bool rbipc_io_validate_batch_lengths(uint32_t max_len, uint32_t count,
                                                        const uint32_t * RBIPC_RESTRICT lens) {
    for (uint32_t i = 0; i < count; ++i) {
        if (RBIPC_UNLIKELY(lens[i] > max_len)) {
            return false;
        }
    }
    return true;
}

/**
 * @brief Atomic procedure: Commit reserved slot batch descriptors in shared memory.
 */
static void rbipc_io_commit_slot_batch(rbipc_ring_t * RBIPC_RESTRICT ring, uint32_t count,
                                                   const uint32_t * RBIPC_RESTRICT tickets,
                                                   const uint32_t * RBIPC_RESTRICT lens) {
    const uint32_t mask = ring->hdr->capacity_mask;
    rbipc_slot_t *slots = (rbipc_slot_t *)RBIPC_ASSUME_ALIGNED(ring->slots, RBIPC_CACHE_LINE);

    for (uint32_t i = 0; i < count; ++i) {
        rbipc_slot_t *slot = &slots[tickets[i] & mask];
        rbipc_slot_commit(slot, tickets[i], lens[i]);
    }
}

int rbipc_commit_write_batch(rbipc_ring_t * RBIPC_RESTRICT ring, uint32_t count,
                             const uint32_t * RBIPC_RESTRICT tickets, const uint32_t * RBIPC_RESTRICT lens) {
    if (RBIPC_UNLIKELY(!ring || !ring->hdr || !tickets || !lens || count == 0)) {
        return RBIPC_ERR_INVAL;
    }

    rbipc_shm_header_t *hdr = ring->hdr;
    if (RBIPC_UNLIKELY(!rbipc_io_validate_batch_lengths(hdr->slot_size, count, lens))) {
        return RBIPC_ERR_INVAL;
    }

    rbipc_io_commit_slot_batch(ring, count, tickets, lens);

    if (RBIPC_UNLIKELY(rbipc_sync_has_waiters(&hdr->futex_waiters))) {
        rbipc_sync_wake_one(&hdr->futex_seq, &hdr->futex_waiters);
    }
    return RBIPC_OK;
}

/**
 * @brief Atomic procedure: Scan contiguous ready slots starting from ticket position.
 */
static uint32_t rbipc_io_scan_ready_slots(const rbipc_ring_t *ring, uint32_t t, uint32_t count) {
    const uint32_t mask = ring->hdr->capacity_mask;
    const rbipc_slot_t *slots = (const rbipc_slot_t *)RBIPC_ASSUME_ALIGNED(ring->slots, RBIPC_CACHE_LINE);
    uint32_t avail = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const rbipc_slot_t *slot = &slots[(t + i) & mask];
        uint32_t seq = atomic_load_explicit(&slot->sequence, memory_order_acquire);
        if (rbipc_slot_is_ready(seq, t + i)) {
            avail++;
        } else {
            break;
        }
    }
    return avail;
}

/**
 * @brief Atomic procedure: Populate read acquisition vector descriptors.
 */
static void rbipc_io_populate_acquired_batch(const rbipc_ring_t * RBIPC_RESTRICT ring,
                                                         uint32_t t, uint32_t count,
                                                         rbipc_rovec_t * RBIPC_RESTRICT rovecs) {
    const uint32_t mask = ring->hdr->capacity_mask;
    const uint32_t slot_size = ring->hdr->slot_size;
    const rbipc_slot_t *slots = (const rbipc_slot_t *)RBIPC_ASSUME_ALIGNED(ring->slots, RBIPC_CACHE_LINE);
    const char *data_base = (const char *)RBIPC_ASSUME_ALIGNED(ring->data_map, RBIPC_CACHE_LINE);

    for (uint32_t i = 0; i < count; ++i) {
        uint32_t ticket_i = t + i;
        const rbipc_slot_t *slot_i = &slots[ticket_i & mask];
        uint32_t state = atomic_load_explicit(&slot_i->state, memory_order_acquire);
        rovecs[i].ticket = ticket_i;
        if (rbipc_slot_state_is_committed(state)) {
            rovecs[i].buf = data_base + ((size_t)(ticket_i & mask) * slot_size);
            rovecs[i].len = atomic_load_explicit(&slot_i->len, memory_order_acquire);
        } else {
            rovecs[i].buf = NULL;
            rovecs[i].len = 0;
        }
    }
    RBIPC_PREFETCH(&slots[(t + count) & mask], 0, 3);
}

int rbipc_read_acquire_batch(rbipc_ring_t * RBIPC_RESTRICT ring, uint32_t count,
                             rbipc_rovec_t * RBIPC_RESTRICT rovecs, uint32_t * RBIPC_RESTRICT out_acquired) {
    if (RBIPC_UNLIKELY(!ring || !ring->hdr || !rovecs || !out_acquired || count == 0)) {
        return RBIPC_ERR_INVAL;
    }

    rbipc_shm_header_t *hdr = ring->hdr;
    if (count > hdr->capacity) {
        count = hdr->capacity;
    }

    uint32_t t = atomic_load_explicit(&hdr->read_ticket, memory_order_relaxed);

    rbipc_sync_state_t sync_state;
    rbipc_sync_state_init(&sync_state, UINT64_MAX);

    for (;;) {
        uint32_t avail = rbipc_io_scan_ready_slots(ring, t, count);
        if (avail > 0) {
            if (rbipc_io_try_claim_read_ticket(hdr, &t, avail)) {
                rbipc_io_populate_acquired_batch(ring, t, avail, rovecs);
                *out_acquired = avail;
                return RBIPC_OK;
            }
            sync_state.spin_count = 0;
        } else {
            if (RBIPC_UNLIKELY(rbipc_ring_is_drained_on_shutdown(hdr, t))) {
                return RBIPC_ERR_SHUTDOWN;
            }

            int rc = rbipc_sync_backoff(&sync_state, &hdr->futex_seq, &hdr->futex_waiters);
            if (RBIPC_UNLIKELY(rc != 0)) {
                return rc;
            }
            t = atomic_load_explicit(&hdr->read_ticket, memory_order_relaxed);
        }
    }
}

/**
 * @brief Atomic procedure: Release batch of consumed slots and advance cycle counter.
 */
static void rbipc_io_release_slot_batch(rbipc_ring_t * RBIPC_RESTRICT ring, uint32_t count,
                                                    const uint32_t * RBIPC_RESTRICT tickets) {
    const uint32_t mask = ring->hdr->capacity_mask;
    const uint32_t cap = ring->hdr->capacity;
    rbipc_slot_t *slots = (rbipc_slot_t *)RBIPC_ASSUME_ALIGNED(ring->slots, RBIPC_CACHE_LINE);

    for (uint32_t i = 0; i < count; ++i) {
        rbipc_slot_t *slot = &slots[tickets[i] & mask];
        rbipc_slot_release(slot, tickets[i], cap);
    }
}

int rbipc_read_release_batch(rbipc_ring_t * RBIPC_RESTRICT ring, uint32_t count,
                             const uint32_t * RBIPC_RESTRICT tickets) {
    if (RBIPC_UNLIKELY(!ring || !ring->hdr || !tickets || count == 0)) {
        return RBIPC_ERR_INVAL;
    }

    rbipc_shm_header_t *hdr = ring->hdr;
    rbipc_io_release_slot_batch(ring, count, tickets);

    if (RBIPC_UNLIKELY(rbipc_sync_has_waiters(&hdr->write_waiters))) {
        rbipc_sync_wake_one(&hdr->write_futex_seq, &hdr->write_waiters);
    }
    return RBIPC_OK;
}
