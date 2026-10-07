/**
 * @file rbipc_io.c
 * @brief Producer and consumer zero-copy reservation, commit, acquire, release routines
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

/**
 * @brief Atomic helper: Try to atomically claim write ticket via compare-and-swap.
 */
RBIPC_INLINE bool rbipc_io_try_claim_write_ticket(rbipc_shm_header_t *hdr, uint32_t *t, uint32_t count) {
    return atomic_compare_exchange_weak_explicit(&hdr->write_ticket, t, *t + count,
                                                 memory_order_relaxed, memory_order_relaxed);
}

/**
 * @brief Atomic helper: Try to atomically claim read ticket via compare-and-swap.
 */
RBIPC_INLINE bool rbipc_io_try_claim_read_ticket(rbipc_shm_header_t *hdr, uint32_t *t, uint32_t count) {
    return atomic_compare_exchange_weak_explicit(&hdr->read_ticket, t, *t + count,
                                                 memory_order_relaxed, memory_order_relaxed);
}

/**
 * @brief Atomic helper: Inspect reserved slot holding producer and recover if process crashed.
 */
static void rbipc_io_check_and_recover_dead_peer(rbipc_shm_header_t *hdr, rbipc_slot_t *slot, uint32_t ticket) {
    uint32_t state = atomic_load_explicit(&slot->state, memory_order_acquire);
    if (rbipc_slot_state_is_reserved(state)) {
        uint32_t pid = atomic_load_explicit(&slot->producer_pid, memory_order_relaxed);
        if (pid > 0 && !rbipc_slot_is_peer_alive((pid_t)pid)) {
            /* Dead peer detected! Atomically transition to POISONED */
            if (rbipc_slot_poison(slot, ticket)) {
                rbipc_sync_wake_one(&hdr->futex_seq, &hdr->futex_waiters);
            }
        }
    }
}

int rbipc_reserve_write_timeout(rbipc_ring_t *ring, uint32_t len, uint64_t timeout_ns,
                                void **out_buf, uint32_t *ticket) {
    if (RBIPC_UNLIKELY(!ring || !ring->hdr || !out_buf || !ticket || len > ring->hdr->slot_size)) {
        return RBIPC_ERR_INVAL;
    }

    rbipc_shm_header_t *hdr = ring->hdr;
    const uint32_t mask = hdr->capacity_mask;
    uint32_t t = atomic_load_explicit(&hdr->write_ticket, memory_order_relaxed);

    rbipc_sync_state_t sync_state;
    rbipc_sync_state_init(&sync_state, timeout_ns);

    for (;;) {
        if (RBIPC_UNLIKELY(rbipc_ring_is_shutdown(hdr))) {
            return RBIPC_ERR_SHUTDOWN;
        }

        rbipc_slot_t *slot = &ring->slots[t & mask];
        uint32_t seq = atomic_load_explicit(&slot->sequence, memory_order_acquire);

        if (RBIPC_LIKELY(rbipc_slot_is_vacant(seq, t))) {
            /* Slot is vacant and ready for write reservation */
            if (RBIPC_LIKELY(rbipc_io_try_claim_write_ticket(hdr, &t, 1))) {
                rbipc_slot_mark_reserved(slot, ring->cached_pid);

                /* Zero-copy virtual pointer calculation */
                *out_buf = rbipc_io_calc_slot_ptr(ring, t);
                *ticket = t;

                /* Prime cache lines for subsequent slot descriptor and data buffer */
                rbipc_io_prefetch_write_next(ring, t + 1);

                return RBIPC_OK;
            }
            /* CAS contention: t updated to latest value; retry immediately */
            sync_state.spin_count = 0;
        } else if (rbipc_seq_is_behind(seq, t)) {
            /* Buffer is full */
            if (timeout_ns == 0) {
                return RBIPC_ERR_FULL;
            }

            int rc = rbipc_sync_backoff(&sync_state, &hdr->write_futex_seq, &hdr->write_waiters);
            if (RBIPC_UNLIKELY(rc != 0)) {
                return rc;
            }
            t = atomic_load_explicit(&hdr->write_ticket, memory_order_relaxed);
        } else {
            /* Another producer claimed ticket t */
            t = atomic_load_explicit(&hdr->write_ticket, memory_order_relaxed);
            sync_state.spin_count = 0;
        }
    }
}

int rbipc_reserve_write(rbipc_ring_t *ring, uint32_t len, void **out_buf, uint32_t *ticket) {
    return rbipc_reserve_write_timeout(ring, len, UINT64_MAX, out_buf, ticket);
}

int rbipc_reserve_write_nonblock(rbipc_ring_t *ring, uint32_t len, void **out_buf, uint32_t *ticket) {
    return rbipc_reserve_write_timeout(ring, len, 0, out_buf, ticket);
}

int rbipc_commit_write(rbipc_ring_t *ring, uint32_t ticket, uint32_t written_len) {
    if (RBIPC_UNLIKELY(!ring || !ring->hdr || written_len > ring->hdr->slot_size)) {
        return RBIPC_ERR_INVAL;
    }

    rbipc_shm_header_t *hdr = ring->hdr;
    rbipc_slot_t *slot = &ring->slots[ticket & hdr->capacity_mask];

    rbipc_slot_commit(slot, ticket, written_len);
    if (RBIPC_UNLIKELY(rbipc_sync_has_waiters(&hdr->futex_waiters))) {
        rbipc_sync_wake_one(&hdr->futex_seq, &hdr->futex_waiters);
    }

    return RBIPC_OK;
}

int rbipc_abort_write(rbipc_ring_t *ring, uint32_t ticket) {
    if (RBIPC_UNLIKELY(!ring || !ring->hdr)) {
        return RBIPC_ERR_INVAL;
    }

    rbipc_shm_header_t *hdr = ring->hdr;
    rbipc_slot_t *slot = &ring->slots[ticket & hdr->capacity_mask];

    if (rbipc_slot_poison(slot, ticket)) {
        rbipc_sync_wake_one(&hdr->futex_seq, &hdr->futex_waiters);
    }

    return RBIPC_OK;
}

int rbipc_read_acquire_timeout(rbipc_ring_t *ring, uint64_t timeout_ns,
                               const void **out_buf, uint32_t *out_len, uint32_t *ticket) {
    if (RBIPC_UNLIKELY(!ring || !ring->hdr || !out_buf || !out_len || !ticket)) {
        return RBIPC_ERR_INVAL;
    }

    rbipc_shm_header_t *hdr = ring->hdr;
    const uint32_t mask = hdr->capacity_mask;
    uint32_t t = atomic_load_explicit(&hdr->read_ticket, memory_order_relaxed);

    rbipc_sync_state_t sync_state;
    rbipc_sync_state_init(&sync_state, timeout_ns);

    for (;;) {
        rbipc_slot_t *slot = &ring->slots[t & mask];
        uint32_t seq = atomic_load_explicit(&slot->sequence, memory_order_acquire);

        if (RBIPC_LIKELY(rbipc_slot_is_ready(seq, t))) {
            /* Slot t is committed or poisoned and ready for consumption */
            if (RBIPC_LIKELY(rbipc_io_try_claim_read_ticket(hdr, &t, 1))) {
                uint32_t state = atomic_load_explicit(&slot->state, memory_order_acquire);
                if (RBIPC_LIKELY(rbipc_slot_state_is_committed(state))) {
                    *out_buf = rbipc_io_calc_slot_ptr(ring, t);
                    *out_len = atomic_load_explicit(&slot->len, memory_order_acquire);
                    *ticket = t;

                    /* Prime cache lines for subsequent slot descriptor and data buffer */
                    rbipc_io_prefetch_read_next(ring, t + 1);

                    return RBIPC_OK;
                } else if (rbipc_slot_state_is_poisoned(state)) {
                    /* Producer crashed or aborted write */
                    *out_buf = NULL;
                    *out_len = 0;
                    *ticket = t;
                    return RBIPC_ERR_POISONED;
                }
            }
            sync_state.spin_count = 0;
        } else if (rbipc_seq_is_behind(seq, t + 1)) {
            /* Slot not yet committed. Probe if holding producer crashed */
            rbipc_io_check_and_recover_dead_peer(hdr, slot, t);

            /* Check shutdown state */
            if (RBIPC_UNLIKELY(rbipc_ring_is_drained_on_shutdown(hdr, t))) {
                return RBIPC_ERR_SHUTDOWN;
            }

            if (timeout_ns == 0) {
                return RBIPC_ERR_EMPTY;
            }

            int rc = rbipc_sync_backoff(&sync_state, &hdr->futex_seq, &hdr->futex_waiters);
            if (RBIPC_UNLIKELY(rc != 0)) {
                return rc;
            }
            t = atomic_load_explicit(&hdr->read_ticket, memory_order_relaxed);
        } else {
            /* Another consumer claimed ticket t */
            t = atomic_load_explicit(&hdr->read_ticket, memory_order_relaxed);
            sync_state.spin_count = 0;
        }
    }
}

int rbipc_read_acquire(rbipc_ring_t *ring, const void **out_buf, uint32_t *out_len, uint32_t *ticket) {
    return rbipc_read_acquire_timeout(ring, UINT64_MAX, out_buf, out_len, ticket);
}

int rbipc_read_acquire_nonblock(rbipc_ring_t *ring, const void **out_buf, uint32_t *out_len, uint32_t *ticket) {
    return rbipc_read_acquire_timeout(ring, 0, out_buf, out_len, ticket);
}

int rbipc_read_release(rbipc_ring_t *ring, uint32_t ticket) {
    if (RBIPC_UNLIKELY(!ring || !ring->hdr)) {
        return RBIPC_ERR_INVAL;
    }

    rbipc_shm_header_t *hdr = ring->hdr;
    rbipc_slot_t *slot = &ring->slots[ticket & hdr->capacity_mask];

    rbipc_slot_release(slot, ticket, hdr->capacity);
    if (RBIPC_UNLIKELY(rbipc_sync_has_waiters(&hdr->write_waiters))) {
        rbipc_sync_wake_one(&hdr->write_futex_seq, &hdr->write_waiters);
    }

    return RBIPC_OK;
}

/**
 * @brief Atomic helper: Count contiguous vacant slots starting from ticket t.
 */
static uint32_t rbipc_io_count_vacant_slots(const rbipc_ring_t *ring, uint32_t t, uint32_t count) {
    const uint32_t mask = ring->hdr->capacity_mask;
    uint32_t avail = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const rbipc_slot_t *slot = &ring->slots[(t + i) & mask];
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
 * @brief Atomic helper: Populate reservation iovecs for a batch of claimed slots.
 */
static void rbipc_io_populate_reserved_batch(rbipc_ring_t *ring, uint32_t t,
                                             uint32_t count, rbipc_iovec_t *iovecs) {
    const uint32_t mask = ring->hdr->capacity_mask;
    const uint32_t slot_size = ring->hdr->slot_size;
    pid_t my_pid = ring->cached_pid;

    for (uint32_t i = 0; i < count; ++i) {
        uint32_t ticket_i = t + i;
        rbipc_slot_t *slot_i = &ring->slots[ticket_i & mask];
        rbipc_slot_mark_reserved(slot_i, my_pid);
        iovecs[i].buf = (char *)ring->data_map + ((size_t)(ticket_i & mask) * slot_size);
        iovecs[i].ticket = ticket_i;
        iovecs[i].max_len = slot_size;
    }
    __builtin_prefetch(&ring->slots[(t + count) & mask], 1, 3);
}

int rbipc_reserve_write_batch(rbipc_ring_t *ring, uint32_t count,
                              rbipc_iovec_t *iovecs, uint32_t *out_reserved) {
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

        uint32_t avail = rbipc_io_count_vacant_slots(ring, t, count);
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

int rbipc_commit_write_batch(rbipc_ring_t *ring, uint32_t count,
                             const uint32_t *tickets, const uint32_t *lens) {
    if (RBIPC_UNLIKELY(!ring || !ring->hdr || !tickets || !lens || count == 0)) {
        return RBIPC_ERR_INVAL;
    }

    rbipc_shm_header_t *hdr = ring->hdr;
    const uint32_t mask = hdr->capacity_mask;

    for (uint32_t i = 0; i < count; ++i) {
        if (RBIPC_UNLIKELY(lens[i] > hdr->slot_size)) {
            return RBIPC_ERR_INVAL;
        }
    }

    for (uint32_t i = 0; i < count; ++i) {
        rbipc_slot_t *slot = &ring->slots[tickets[i] & mask];
        rbipc_slot_commit(slot, tickets[i], lens[i]);
    }

    if (RBIPC_UNLIKELY(rbipc_sync_has_waiters(&hdr->futex_waiters))) {
        rbipc_sync_wake_one(&hdr->futex_seq, &hdr->futex_waiters);
    }
    return RBIPC_OK;
}

/**
 * @brief Atomic helper: Count contiguous ready slots starting from ticket t.
 */
static uint32_t rbipc_io_count_ready_slots(const rbipc_ring_t *ring, uint32_t t, uint32_t count) {
    const uint32_t mask = ring->hdr->capacity_mask;
    uint32_t avail = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const rbipc_slot_t *slot = &ring->slots[(t + i) & mask];
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
 * @brief Atomic helper: Populate acquisition rovecs for a batch of claimed slots.
 */
static void rbipc_io_populate_acquired_batch(const rbipc_ring_t *ring, uint32_t t,
                                             uint32_t count, rbipc_rovec_t *rovecs) {
    const uint32_t mask = ring->hdr->capacity_mask;
    const uint32_t slot_size = ring->hdr->slot_size;

    for (uint32_t i = 0; i < count; ++i) {
        uint32_t ticket_i = t + i;
        rbipc_slot_t *slot_i = &ring->slots[ticket_i & mask];
        uint32_t state = atomic_load_explicit(&slot_i->state, memory_order_acquire);
        rovecs[i].ticket = ticket_i;
        if (rbipc_slot_state_is_committed(state)) {
            rovecs[i].buf = (const char *)ring->data_map + ((size_t)(ticket_i & mask) * slot_size);
            rovecs[i].len = atomic_load_explicit(&slot_i->len, memory_order_acquire);
        } else {
            rovecs[i].buf = NULL;
            rovecs[i].len = 0;
        }
    }
    __builtin_prefetch(&ring->slots[(t + count) & mask], 0, 3);
}

int rbipc_read_acquire_batch(rbipc_ring_t *ring, uint32_t count,
                             rbipc_rovec_t *rovecs, uint32_t *out_acquired) {
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
        uint32_t avail = rbipc_io_count_ready_slots(ring, t, count);
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

int rbipc_read_release_batch(rbipc_ring_t *ring, uint32_t count, const uint32_t *tickets) {
    if (RBIPC_UNLIKELY(!ring || !ring->hdr || !tickets || count == 0)) {
        return RBIPC_ERR_INVAL;
    }

    rbipc_shm_header_t *hdr = ring->hdr;
    const uint32_t mask = hdr->capacity_mask;

    for (uint32_t i = 0; i < count; ++i) {
        rbipc_slot_t *slot = &ring->slots[tickets[i] & mask];
        rbipc_slot_release(slot, tickets[i], hdr->capacity);
    }

    if (RBIPC_UNLIKELY(rbipc_sync_has_waiters(&hdr->write_waiters))) {
        rbipc_sync_wake_one(&hdr->write_futex_seq, &hdr->write_waiters);
    }
    return RBIPC_OK;
}
