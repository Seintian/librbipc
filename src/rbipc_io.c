/**
 * @file rbipc_io.c
 * @brief Producer and consumer zero-copy reservation, commit, acquire, release routines
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "rbipc.h"
#include "rbipc_internal.h"
#include "rbipc_math.h"
#include "rbipc_slot.h"
#include "rbipc_sync.h"

#include <unistd.h>
#include <errno.h>

#define PEER_CHECK_INTERVAL 500

int rbipc_reserve_write_timeout(rbipc_ring_t *ring, uint32_t len, uint64_t timeout_ns,
                                void **out_buf, uint32_t *ticket) {
    if (!ring || !ring->hdr || !out_buf || !ticket || len > ring->hdr->slot_size) {
        return RBIPC_ERR_INVAL;
    }

    rbipc_shm_header_t *hdr = ring->hdr;
    const uint32_t mask = hdr->capacity_mask;
    uint32_t t = atomic_load_explicit(&hdr->write_ticket, memory_order_relaxed);

    rbipc_sync_state_t sync_state;
    rbipc_sync_state_init(&sync_state, timeout_ns);

    for (;;) {
        if (atomic_load_explicit(&hdr->shutdown_flag, memory_order_relaxed)) {
            return RBIPC_ERR_SHUTDOWN;
        }

        rbipc_slot_t *slot = &ring->slots[t & mask];
        uint32_t seq = atomic_load_explicit(&slot->sequence, memory_order_acquire);
        int32_t diff = rbipc_seq_diff(seq, t);

        if (diff == 0) {
            /* Slot is vacant and ready for write reservation */
            if (atomic_compare_exchange_weak_explicit(&hdr->write_ticket, &t, t + 1,
                                                      memory_order_relaxed, memory_order_relaxed)) {
                rbipc_slot_mark_reserved(slot, getpid());

                /* Zero-copy virtual pointer calculation */
                *out_buf = (char *)ring->data_map + ((size_t)(t & mask) * hdr->slot_size);
                *ticket = t;
                return RBIPC_OK;
            }
            /* CAS contention: t updated to latest value; retry immediately */
            sync_state.spin_count = 0;
        } else if (diff < 0) {
            /* Buffer is full */
            if (timeout_ns == 0) {
                return RBIPC_ERR_FULL;
            }

            int rc = rbipc_sync_backoff(&sync_state, &hdr->futex_seq);
            if (rc != 0) {
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
    if (!ring || !ring->hdr || written_len > ring->hdr->slot_size) {
        return RBIPC_ERR_INVAL;
    }

    rbipc_shm_header_t *hdr = ring->hdr;
    rbipc_slot_t *slot = &ring->slots[ticket & hdr->capacity_mask];

    rbipc_slot_commit(slot, ticket, written_len);
    rbipc_sync_wake_one(&hdr->futex_seq);

    return RBIPC_OK;
}

int rbipc_abort_write(rbipc_ring_t *ring, uint32_t ticket) {
    if (!ring || !ring->hdr) {
        return RBIPC_ERR_INVAL;
    }

    rbipc_shm_header_t *hdr = ring->hdr;
    rbipc_slot_t *slot = &ring->slots[ticket & hdr->capacity_mask];

    if (rbipc_slot_poison(slot, ticket)) {
        rbipc_sync_wake_one(&hdr->futex_seq);
    }

    return RBIPC_OK;
}

int rbipc_read_acquire_timeout(rbipc_ring_t *ring, uint64_t timeout_ns,
                               const void **out_buf, uint32_t *out_len, uint32_t *ticket) {
    if (!ring || !ring->hdr || !out_buf || !out_len || !ticket) {
        return RBIPC_ERR_INVAL;
    }

    rbipc_shm_header_t *hdr = ring->hdr;
    const uint32_t mask = hdr->capacity_mask;
    uint32_t t = atomic_load_explicit(&hdr->read_ticket, memory_order_relaxed);

    rbipc_sync_state_t sync_state;
    rbipc_sync_state_init(&sync_state, timeout_ns);

    uint32_t reserved_wait_count = 0;

    for (;;) {
        rbipc_slot_t *slot = &ring->slots[t & mask];
        uint32_t seq = atomic_load_explicit(&slot->sequence, memory_order_acquire);
        int32_t diff = rbipc_seq_diff(seq, t + 1);

        if (diff == 0) {
            /* Slot t is committed or poisoned and ready for consumption */
            if (atomic_compare_exchange_weak_explicit(&hdr->read_ticket, &t, t + 1,
                                                      memory_order_relaxed, memory_order_relaxed)) {
                uint32_t state = atomic_load_explicit(&slot->state, memory_order_acquire);
                if (state == RBIPC_SLOT_COMMITTED) {
                    *out_buf = (const char *)ring->data_map + ((size_t)(t & mask) * hdr->slot_size);
                    *out_len = atomic_load_explicit(&slot->len, memory_order_acquire);
                    *ticket = t;
                    return RBIPC_OK;
                } else if (state == RBIPC_SLOT_POISONED) {
                    /* Producer crashed or aborted write */
                    *out_buf = NULL;
                    *out_len = 0;
                    *ticket = t;
                    return RBIPC_ERR_POISONED;
                }
            }
            sync_state.spin_count = 0;
            reserved_wait_count = 0;
        } else if (diff < 0) {
            /* Slot not yet committed. Probe if holding producer crashed */
            uint32_t state = atomic_load_explicit(&slot->state, memory_order_acquire);
            if (state == RBIPC_SLOT_RESERVED) {
                reserved_wait_count++;
                if (reserved_wait_count >= PEER_CHECK_INTERVAL) {
                    uint32_t pid = atomic_load_explicit(&slot->producer_pid, memory_order_relaxed);
                    if (pid > 0 && !rbipc_slot_is_peer_alive((pid_t)pid)) {
                        /* Dead peer detected! Atomically transition to POISONED */
                        if (rbipc_slot_poison(slot, t)) {
                            rbipc_sync_wake_one(&hdr->futex_seq);
                        }
                    }
                    reserved_wait_count = 0;
                }
            } else {
                reserved_wait_count = 0;
            }

            /* Check shutdown state */
            if (atomic_load_explicit(&hdr->shutdown_flag, memory_order_relaxed)) {
                uint32_t cur_write = atomic_load_explicit(&hdr->write_ticket, memory_order_relaxed);
                if (t >= cur_write) {
                    return RBIPC_ERR_SHUTDOWN;
                }
            }

            if (timeout_ns == 0) {
                return RBIPC_ERR_EMPTY;
            }

            int rc = rbipc_sync_backoff(&sync_state, &hdr->futex_seq);
            if (rc != 0) {
                return rc;
            }
            t = atomic_load_explicit(&hdr->read_ticket, memory_order_relaxed);
        } else {
            /* Another consumer claimed ticket t */
            t = atomic_load_explicit(&hdr->read_ticket, memory_order_relaxed);
            sync_state.spin_count = 0;
            reserved_wait_count = 0;
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
    if (!ring || !ring->hdr) {
        return RBIPC_ERR_INVAL;
    }

    rbipc_shm_header_t *hdr = ring->hdr;
    rbipc_slot_t *slot = &ring->slots[ticket & hdr->capacity_mask];

    rbipc_slot_release(slot, ticket, hdr->capacity);
    rbipc_sync_wake_one(&hdr->futex_seq);

    return RBIPC_OK;
}
