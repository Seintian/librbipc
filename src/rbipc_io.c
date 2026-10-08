/**
 * @file rbipc_io.c
 * @brief Zero-copy input/output queueing operations, ticket coordination, and batching.
 * @details Implements the lock-free single-item and vectorized multi-item reservation,
 *          commit, acquisition, and release pipelines. Provides crash detection for
 *          uncommitted peer slots, hybrid adaptive spin-then-wait futex synchronization,
 *          and memory prefetching.
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
 * @brief Attempts an atomic CAS reservation of a producer write ticket range.
 * @details Uses @c atomic_compare_exchange_weak_explicit with relaxed memory order
 *          to advance @c write_ticket by @p count. If the CAS fails, @p t is updated
 *          with the most recent observed ticket value.
 *
 * @param[in,out] hdr   Pointer to shared memory control header.
 * @param[in,out] t     Pointer to expected current write ticket.
 * @param[in]     count Number of consecutive slots to reserve.
 *
 * @return @c true if the ticket range was claimed successfully; @c false otherwise.
 */
RBIPC_INLINE bool rbipc_io_try_claim_write_ticket(rbipc_shm_header_t * RBIPC_RESTRICT hdr,
                                                  uint32_t * RBIPC_RESTRICT t,
                                                  uint32_t count) {
    return atomic_compare_exchange_weak_explicit(&hdr->write_ticket, t, *t + count,
                                                 memory_order_relaxed, memory_order_relaxed);
}

/**
 * @brief Attempts an atomic CAS reservation of a consumer read ticket range.
 * @details Uses @c atomic_compare_exchange_weak_explicit with relaxed memory order
 *          to advance @c read_ticket by @p count. If the CAS fails, @p t is updated
 *          with the most recent observed ticket value.
 *
 * @param[in,out] hdr   Pointer to shared memory control header.
 * @param[in,out] t     Pointer to expected current read ticket.
 * @param[in]     count Number of consecutive slots to acquire.
 *
 * @return @c true if the ticket range was claimed successfully; @c false otherwise.
 */
RBIPC_INLINE bool rbipc_io_try_claim_read_ticket(rbipc_shm_header_t * RBIPC_RESTRICT hdr,
                                                 uint32_t * RBIPC_RESTRICT t,
                                                 uint32_t count) {
    return atomic_compare_exchange_weak_explicit(&hdr->read_ticket, t, *t + count,
                                                 memory_order_relaxed, memory_order_relaxed);
}

/**
 * @brief Audits a slot descriptor and recovers the ring if the holding producer died prematurely.
 * @details If a slot remains in @c RBIPC_SLOT_RESERVED state and its holding PID is no longer
 *          alive in the operating system process table, poisons the slot and wakes waiting
 *          consumers so progress is not permanently stalled.
 *
 * @param[in,out] hdr    Pointer to shared memory control header.
 * @param[in,out] slot   Pointer to slot descriptor under inspection.
 * @param[in]     ticket Current consumer ticket value.
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
 * @brief Finalizes a slot write reservation and primes CPU caches.
 * @details Marks the slot reserved with the current PID, populates the output buffer
 *          and ticket pointers, and issues prefetch instructions for the next slot.
 *
 * @param[in]  ring       Pointer to active ring handle.
 * @param[out] slot       Pointer to slot descriptor being reserved.
 * @param[in]  ticket     Claimed write ticket.
 * @param[out] out_buf    Pointer to receive the payload address.
 * @param[out] out_ticket Pointer to receive the claimed ticket.
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
 * @brief Evaluates ring full conditions and executes adaptive futex backoff.
 * @details If timeout is zero, returns @c RBIPC_ERR_FULL immediately. Otherwise,
 *          delegates to rbipc_sync_backoff() waiting on @c write_futex_seq, and refreshes
 *          the producer ticket upon wakeup.
 *
 * @param[in,out] sync_state State tracking spin iterations and elapsed timeout.
 * @param[in,out] hdr        Shared memory control header.
 * @param[in]     timeout_ns Nanosecond timeout budget.
 * @param[out]    t          Pointer to receive refreshed write ticket.
 *
 * @return Status code indicating the outcome of the backoff.
 * @retval RBIPC_OK           Woken up before timeout.
 * @retval RBIPC_ERR_FULL     Non-blocking mode and buffer is full.
 * @retval RBIPC_ERR_TIMEDOUT Timeout expired without available space.
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
 * @brief Finalizes committed read acquisition and extracts message payload pointers.
 * @details Inspects slot state. If committed, returns payload buffer and length.
 *          If poisoned, returns @c RBIPC_ERR_POISONED. Otherwise, returns @c RBIPC_ERR_BUSY.
 *
 * @param[in]  ring       Pointer to ring handle.
 * @param[in]  slot       Pointer to acquired slot descriptor.
 * @param[in]  ticket     Acquired read ticket.
 * @param[out] out_buf    Pointer to receive payload address.
 * @param[out] out_len    Pointer to receive payload length.
 * @param[out] out_ticket Pointer to receive ticket.
 *
 * @return Status code indicating the outcome of acquisition.
 * @retval RBIPC_OK           Slot payload successfully acquired.
 * @retval RBIPC_ERR_POISONED Slot was poisoned due to producer crash.
 * @retval RBIPC_ERR_BUSY     Slot write in progress.
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
 * @brief Evaluates ring empty conditions and executes adaptive futex backoff.
 * @details Checks for crashed producer peers, checks shutdown drain condition,
 *          and either returns @c RBIPC_ERR_EMPTY (non-blocking) or executes futex wait.
 *
 * @param[in,out] sync_state  State tracking spin iterations and elapsed timeout.
 * @param[in,out] hdr         Shared memory control header.
 * @param[in,out] slot        Current slot descriptor.
 * @param[in]     t           Current consumer ticket.
 * @param[in]     timeout_ns  Nanosecond timeout budget.
 * @param[out]    out_next_t  Pointer to receive refreshed read ticket.
 *
 * @return Status code indicating the outcome of the backoff.
 * @retval RBIPC_OK           Woken up with data available.
 * @retval RBIPC_ERR_EMPTY    Non-blocking mode and ring is empty.
 * @retval RBIPC_ERR_SHUTDOWN Ring shut down and all committed data consumed.
 * @retval RBIPC_ERR_TIMEDOUT Timeout expired without available data.
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

/**
 * @brief Reserves a slot for writing with a specified nanosecond timeout.
 * @details Scans for a vacant slot at the current producer ticket. Uses an adaptive
 *          spin-then-wait loop with futex suspension if the ring is full.
 */
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

/**
 * @brief Reserves a slot for writing with indefinite blocking.
 */
int rbipc_reserve_write(rbipc_ring_t * RBIPC_RESTRICT ring, uint32_t len,
                        void ** RBIPC_RESTRICT out_buf, uint32_t * RBIPC_RESTRICT ticket) {
    return rbipc_reserve_write_timeout(ring, len, UINT64_MAX, out_buf, ticket);
}

/**
 * @brief Reserves a slot for writing non-blockingly.
 */
int rbipc_reserve_write_nonblock(rbipc_ring_t * RBIPC_RESTRICT ring, uint32_t len,
                                 void ** RBIPC_RESTRICT out_buf, uint32_t * RBIPC_RESTRICT ticket) {
    return rbipc_reserve_write_timeout(ring, len, 0, out_buf, ticket);
}

/**
 * @brief Commits a previously reserved slot, publishing its contents to consumers.
 * @details Stores the written length, publishes the slot sequence with release memory semantics,
 *          and wakes a suspended consumer via futex if waiters exist.
 */
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

/**
 * @brief Aborts a reserved slot without committing meaningful payload.
 * @details Transitions the slot to poisoned state and advances sequence, allowing
 *          consumers to skip the slot without stalling progress.
 */
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

/**
 * @brief Acquires the next committed slot for reading with a nanosecond timeout.
 * @details Reads the next slot at the consumer ticket. Supports spin-then-wait backoff
 *          with futex suspension if no data is currently available.
 */
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

/**
 * @brief Acquires the next committed slot for reading with indefinite blocking.
 */
int rbipc_read_acquire(rbipc_ring_t * RBIPC_RESTRICT ring, const void ** RBIPC_RESTRICT out_buf,
                       uint32_t * RBIPC_RESTRICT out_len, uint32_t * RBIPC_RESTRICT ticket) {
    return rbipc_read_acquire_timeout(ring, UINT64_MAX, out_buf, out_len, ticket);
}

/**
 * @brief Acquires the next committed slot for reading non-blockingly.
 */
int rbipc_read_acquire_nonblock(rbipc_ring_t * RBIPC_RESTRICT ring, const void ** RBIPC_RESTRICT out_buf,
                                uint32_t * RBIPC_RESTRICT out_len, uint32_t * RBIPC_RESTRICT ticket) {
    return rbipc_read_acquire_timeout(ring, 0, out_buf, out_len, ticket);
}

/**
 * @brief Releases a previously acquired slot, recycling it for future producer writes.
 * @details Increments the slot sequence by capacity to mark it vacant for the next cycle,
 *          and wakes a suspended producer via futex if write waiters exist.
 */
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
 * @brief Scans a contiguous range of vacant slots starting from ticket position.
 * @details Iterates up to @p count slots, returning the number of consecutive vacant slots found.
 *
 * @param[in] ring  Pointer to ring handle.
 * @param[in] t     Starting producer ticket.
 * @param[in] count Maximum number of slots to examine.
 *
 * @return Number of contiguous vacant slots available.
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
 * @brief Populates write reservation vector descriptors for an allocated batch.
 * @details Marks each reserved slot with caller PID, assigns buffer pointers and tickets,
 *          and prefetches upcoming slots.
 *
 * @param[in,out] ring   Pointer to ring handle.
 * @param[in]     t      Base ticket claimed for the batch.
 * @param[in]     count  Number of reserved slots in batch.
 * @param[out]    iovecs Array of write I/O vectors to populate.
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

/**
 * @brief Reserves a contiguous batch of slots for writing using vector descriptors.
 * @details Claims up to @p count contiguous vacant slots in a single atomic CAS operation.
 */
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
 * @brief Validates that all batch message lengths do not exceed slot capacity.
 *
 * @param[in] max_len Maximum slot payload limit.
 * @param[in] count   Number of elements in array.
 * @param[in] lens    Array of payload lengths to validate.
 *
 * @return @c true if all lengths are valid; @c false if any exceeds @p max_len.
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
 * @brief Commits an array of reserved slot descriptors in shared memory.
 *
 * @param[in,out] ring    Pointer to ring handle.
 * @param[in]     count   Number of elements in batch.
 * @param[in]     tickets Array of reservation tickets to commit.
 * @param[in]     lens    Array of written payload lengths.
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

/**
 * @brief Commits a batch of previously reserved slots, publishing all payloads to consumers.
 */
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
 * @brief Scans a contiguous range of ready committed slots starting from ticket position.
 *
 * @param[in] ring  Pointer to ring handle.
 * @param[in] t     Starting consumer ticket.
 * @param[in] count Maximum number of slots to examine.
 *
 * @return Number of contiguous ready slots available.
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
 * @brief Populates read acquisition vector descriptors for an acquired batch.
 *
 * @param[in]  ring   Pointer to ring handle.
 * @param[in]  t      Base ticket claimed for the batch.
 * @param[in]  count  Number of acquired slots in batch.
 * @param[out] rovecs Array of read-only I/O vectors to populate.
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

/**
 * @brief Acquires a contiguous batch of committed slots for reading using vector descriptors.
 */
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
 * @brief Releases a batch of consumed slots and recycles them for producer reuse.
 *
 * @param[in,out] ring    Pointer to ring handle.
 * @param[in]     count   Number of elements in batch.
 * @param[in]     tickets Array of slot tickets to release.
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

/**
 * @brief Releases a batch of previously acquired slots, recycling them for future producer writes.
 */
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
