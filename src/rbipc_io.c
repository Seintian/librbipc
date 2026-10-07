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
        if (__builtin_expect(atomic_load_explicit(&hdr->shutdown_flag, memory_order_relaxed) != 0, 0)) {
            return RBIPC_ERR_SHUTDOWN;
        }

        rbipc_slot_t *slot = &ring->slots[t & mask];
        uint32_t seq = atomic_load_explicit(&slot->sequence, memory_order_acquire);
        int32_t diff = rbipc_seq_diff(seq, t);

        if (__builtin_expect(diff == 0, 1)) {
            /* Slot is vacant and ready for write reservation */
            if (__builtin_expect(atomic_compare_exchange_weak_explicit(&hdr->write_ticket, &t, t + 1,
                                                                      memory_order_relaxed, memory_order_relaxed), 1)) {
                rbipc_slot_mark_reserved(slot, ring->cached_pid);

                /* Zero-copy virtual pointer calculation */
                *out_buf = (char *)ring->data_map + ((size_t)(t & mask) * hdr->slot_size);
                *ticket = t;

                /* Software prefetching: prime cache lines for subsequent slot descriptor and data buffer */
                __builtin_prefetch(&ring->slots[(t + 1) & mask], 1, 3);
                __builtin_prefetch((char *)ring->data_map + ((size_t)((t + 1) & mask) * hdr->slot_size), 1, 1);

                return RBIPC_OK;
            }
            /* CAS contention: t updated to latest value; retry immediately */
            sync_state.spin_count = 0;
        } else if (diff < 0) {
            /* Buffer is full */
            if (timeout_ns == 0) {
                return RBIPC_ERR_FULL;
            }

            int rc = rbipc_sync_backoff(&sync_state, &hdr->write_futex_seq, &hdr->write_waiters);
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
    if (__builtin_expect(atomic_load_explicit(&hdr->futex_waiters, memory_order_seq_cst) > 0, 0)) {
        rbipc_sync_wake_one(&hdr->futex_seq, &hdr->futex_waiters);
    }

    return RBIPC_OK;
}

int rbipc_abort_write(rbipc_ring_t *ring, uint32_t ticket) {
    if (!ring || !ring->hdr) {
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
    if (!ring || !ring->hdr || !out_buf || !out_len || !ticket) {
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
        int32_t diff = rbipc_seq_diff(seq, t + 1);

        if (__builtin_expect(diff == 0, 1)) {
            /* Slot t is committed or poisoned and ready for consumption */
            if (__builtin_expect(atomic_compare_exchange_weak_explicit(&hdr->read_ticket, &t, t + 1,
                                                                      memory_order_relaxed, memory_order_relaxed), 1)) {
                uint32_t state = atomic_load_explicit(&slot->state, memory_order_acquire);
                if (__builtin_expect(state == RBIPC_SLOT_COMMITTED, 1)) {
                    *out_buf = (const char *)ring->data_map + ((size_t)(t & mask) * hdr->slot_size);
                    *out_len = atomic_load_explicit(&slot->len, memory_order_acquire);
                    *ticket = t;

                    /* Software prefetching: prime cache lines for subsequent slot descriptor and data buffer */
                    __builtin_prefetch(&ring->slots[(t + 1) & mask], 0, 3);
                    __builtin_prefetch((const char *)ring->data_map + ((size_t)((t + 1) & mask) * hdr->slot_size), 0, 1);

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
        } else if (diff < 0) {
            /* Slot not yet committed. Probe if holding producer crashed */
            uint32_t state = atomic_load_explicit(&slot->state, memory_order_acquire);
            if (state == RBIPC_SLOT_RESERVED) {
                uint32_t pid = atomic_load_explicit(&slot->producer_pid, memory_order_relaxed);
                if (pid > 0 && !rbipc_slot_is_peer_alive((pid_t)pid)) {
                    /* Dead peer detected! Atomically transition to POISONED */
                    if (rbipc_slot_poison(slot, t)) {
                        rbipc_sync_wake_one(&hdr->futex_seq, &hdr->futex_waiters);
                    }
                }
            }

            /* Check shutdown state */
            if (__builtin_expect(atomic_load_explicit(&hdr->shutdown_flag, memory_order_relaxed) != 0, 0)) {
                uint32_t cur_write = atomic_load_explicit(&hdr->write_ticket, memory_order_relaxed);
                if (t >= cur_write) {
                    return RBIPC_ERR_SHUTDOWN;
                }
            }

            if (timeout_ns == 0) {
                return RBIPC_ERR_EMPTY;
            }

            int rc = rbipc_sync_backoff(&sync_state, &hdr->futex_seq, &hdr->futex_waiters);
            if (rc != 0) {
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
    if (!ring || !ring->hdr) {
        return RBIPC_ERR_INVAL;
    }

    rbipc_shm_header_t *hdr = ring->hdr;
    rbipc_slot_t *slot = &ring->slots[ticket & hdr->capacity_mask];

    rbipc_slot_release(slot, ticket, hdr->capacity);
    if (__builtin_expect(atomic_load_explicit(&hdr->write_waiters, memory_order_seq_cst) > 0, 0)) {
        rbipc_sync_wake_one(&hdr->write_futex_seq, &hdr->write_waiters);
    }

    return RBIPC_OK;
}

int rbipc_reserve_write_batch(rbipc_ring_t *ring, uint32_t count,
                              rbipc_iovec_t *iovecs, uint32_t *out_reserved) {
    if (!ring || !ring->hdr || !iovecs || !out_reserved || count == 0) {
        return RBIPC_ERR_INVAL;
    }

    rbipc_shm_header_t *hdr = ring->hdr;
    if (count > hdr->capacity) {
        count = hdr->capacity;
    }

    const uint32_t mask = hdr->capacity_mask;
    uint32_t t = atomic_load_explicit(&hdr->write_ticket, memory_order_relaxed);

    rbipc_sync_state_t sync_state;
    rbipc_sync_state_init(&sync_state, UINT64_MAX);

    pid_t my_pid = ring->cached_pid;

    for (;;) {
        if (__builtin_expect(atomic_load_explicit(&hdr->shutdown_flag, memory_order_relaxed) != 0, 0)) {
            return RBIPC_ERR_SHUTDOWN;
        }

        uint32_t avail = 0;
        for (uint32_t i = 0; i < count; ++i) {
            rbipc_slot_t *slot = &ring->slots[(t + i) & mask];
            uint32_t seq = atomic_load_explicit(&slot->sequence, memory_order_acquire);
            int32_t diff = rbipc_seq_diff(seq, t + i);
            if (diff == 0) {
                avail++;
            } else {
                break;
            }
        }

        if (avail > 0) {
            if (atomic_compare_exchange_weak_explicit(&hdr->write_ticket, &t, t + avail,
                                                      memory_order_relaxed, memory_order_relaxed)) {
                for (uint32_t i = 0; i < avail; ++i) {
                    uint32_t ticket_i = t + i;
                    rbipc_slot_t *slot_i = &ring->slots[ticket_i & mask];
                    rbipc_slot_mark_reserved(slot_i, my_pid);
                    iovecs[i].buf = (char *)ring->data_map + ((size_t)(ticket_i & mask) * hdr->slot_size);
                    iovecs[i].ticket = ticket_i;
                    iovecs[i].max_len = hdr->slot_size;
                }
                __builtin_prefetch(&ring->slots[(t + avail) & mask], 1, 3);
                *out_reserved = avail;
                return RBIPC_OK;
            }
            sync_state.spin_count = 0;
        } else {
            int rc = rbipc_sync_backoff(&sync_state, &hdr->write_futex_seq, &hdr->write_waiters);
            if (rc != 0) {
                return rc;
            }
            t = atomic_load_explicit(&hdr->write_ticket, memory_order_relaxed);
        }
    }
}

int rbipc_commit_write_batch(rbipc_ring_t *ring, uint32_t count,
                             const uint32_t *tickets, const uint32_t *lens) {
    if (!ring || !ring->hdr || !tickets || !lens || count == 0) {
        return RBIPC_ERR_INVAL;
    }

    rbipc_shm_header_t *hdr = ring->hdr;
    const uint32_t mask = hdr->capacity_mask;

    for (uint32_t i = 0; i < count; ++i) {
        if (lens[i] > hdr->slot_size) {
            return RBIPC_ERR_INVAL;
        }
    }

    for (uint32_t i = 0; i < count; ++i) {
        rbipc_slot_t *slot = &ring->slots[tickets[i] & mask];
        rbipc_slot_commit(slot, tickets[i], lens[i]);
    }

    if (__builtin_expect(atomic_load_explicit(&hdr->futex_waiters, memory_order_seq_cst) > 0, 0)) {
        rbipc_sync_wake_one(&hdr->futex_seq, &hdr->futex_waiters);
    }
    return RBIPC_OK;
}

int rbipc_read_acquire_batch(rbipc_ring_t *ring, uint32_t count,
                             rbipc_rovec_t *rovecs, uint32_t *out_acquired) {
    if (!ring || !ring->hdr || !rovecs || !out_acquired || count == 0) {
        return RBIPC_ERR_INVAL;
    }

    rbipc_shm_header_t *hdr = ring->hdr;
    if (count > hdr->capacity) {
        count = hdr->capacity;
    }

    const uint32_t mask = hdr->capacity_mask;
    uint32_t t = atomic_load_explicit(&hdr->read_ticket, memory_order_relaxed);

    rbipc_sync_state_t sync_state;
    rbipc_sync_state_init(&sync_state, UINT64_MAX);

    for (;;) {
        uint32_t avail = 0;
        for (uint32_t i = 0; i < count; ++i) {
            rbipc_slot_t *slot = &ring->slots[(t + i) & mask];
            uint32_t seq = atomic_load_explicit(&slot->sequence, memory_order_acquire);
            int32_t diff = rbipc_seq_diff(seq, t + i + 1);
            if (diff == 0) {
                avail++;
            } else {
                break;
            }
        }

        if (avail > 0) {
            if (atomic_compare_exchange_weak_explicit(&hdr->read_ticket, &t, t + avail,
                                                      memory_order_relaxed, memory_order_relaxed)) {
                for (uint32_t i = 0; i < avail; ++i) {
                    uint32_t ticket_i = t + i;
                    rbipc_slot_t *slot_i = &ring->slots[ticket_i & mask];
                    uint32_t state = atomic_load_explicit(&slot_i->state, memory_order_acquire);
                    rovecs[i].ticket = ticket_i;
                    if (state == RBIPC_SLOT_COMMITTED) {
                        rovecs[i].buf = (const char *)ring->data_map + ((size_t)(ticket_i & mask) * hdr->slot_size);
                        rovecs[i].len = atomic_load_explicit(&slot_i->len, memory_order_acquire);
                    } else {
                        rovecs[i].buf = NULL;
                        rovecs[i].len = 0;
                    }
                }
                __builtin_prefetch(&ring->slots[(t + avail) & mask], 0, 3);
                *out_acquired = avail;
                return RBIPC_OK;
            }
            sync_state.spin_count = 0;
        } else {
            if (__builtin_expect(atomic_load_explicit(&hdr->shutdown_flag, memory_order_relaxed) != 0, 0)) {
                uint32_t cur_write = atomic_load_explicit(&hdr->write_ticket, memory_order_relaxed);
                if (t >= cur_write) {
                    return RBIPC_ERR_SHUTDOWN;
                }
            }

            int rc = rbipc_sync_backoff(&sync_state, &hdr->futex_seq, &hdr->futex_waiters);
            if (rc != 0) {
                return rc;
            }
            t = atomic_load_explicit(&hdr->read_ticket, memory_order_relaxed);
        }
    }
}

int rbipc_read_release_batch(rbipc_ring_t *ring, uint32_t count, const uint32_t *tickets) {
    if (!ring || !ring->hdr || !tickets || count == 0) {
        return RBIPC_ERR_INVAL;
    }

    rbipc_shm_header_t *hdr = ring->hdr;
    const uint32_t mask = hdr->capacity_mask;

    for (uint32_t i = 0; i < count; ++i) {
        rbipc_slot_t *slot = &ring->slots[tickets[i] & mask];
        rbipc_slot_release(slot, tickets[i], hdr->capacity);
    }

    if (__builtin_expect(atomic_load_explicit(&hdr->write_waiters, memory_order_seq_cst) > 0, 0)) {
        rbipc_sync_wake_one(&hdr->write_futex_seq, &hdr->write_waiters);
    }
    return RBIPC_OK;
}
