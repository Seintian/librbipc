/**
 * @file rbipc_pubsub.c
 * @brief Implementation of 1-to-N multicast publish-subscribe engine.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "rbipc_pubsub.h"
#include "rbipc_internal.h"
#include "rbipc_predicate.h"
#include "rbipc_sync.h"

#include <stdlib.h>
#include <string.h>

struct rbipc_sub {
    rbipc_ring_t *ring;
    uint64_t cursor;
};

RBIPC_LEAF
int rbipc_publish(rbipc_ring_t *ring, const void *buf, uint32_t len) {
    if (RBIPC_UNLIKELY(!ring || !ring->hdr || !buf)) {
        return RBIPC_ERR_INVAL;
    }
    rbipc_shm_header_t *hdr = ring->hdr;
    if (RBIPC_UNLIKELY(len > hdr->slot_size)) {
        return RBIPC_ERR_INVAL;
    }

    uint32_t mask = hdr->capacity_mask;
    rbipc_slot_t *slots = (rbipc_slot_t *)RBIPC_ASSUME_ALIGNED(ring->slots, RBIPC_CACHE_LINE);

    uint64_t ticket = atomic_fetch_add_explicit(&hdr->write_ticket, 1, memory_order_relaxed);
    rbipc_slot_t *slot = &slots[ticket & mask];

    atomic_store_explicit(&slot->producer_pid, (uint32_t)ring->cached_pid, memory_order_relaxed);
    atomic_store_explicit(&slot->consumer_pid, 0, memory_order_relaxed);
    atomic_store_explicit(&slot->acquire_timestamp_ns, 0, memory_order_relaxed);
    atomic_store_explicit(&slot->state, RBIPC_SLOT_RESERVED, memory_order_relaxed);

    char *slot_ptr = (char *)ring->data_map + ((size_t)(ticket & mask) * hdr->slot_size);
    memcpy(slot_ptr, buf, len);

    atomic_store_explicit(&slot->len, len, memory_order_relaxed);
    atomic_store_explicit(&slot->state, RBIPC_SLOT_COMMITTED, memory_order_release);
    atomic_store_explicit(&slot->sequence, ticket + 1, memory_order_release);

    atomic_thread_fence(memory_order_seq_cst);
    if (RBIPC_UNLIKELY(rbipc_sync_has_waiters(&hdr->futex_waiters))) {
        rbipc_sync_wake_all(&hdr->futex_seq, &hdr->futex_waiters);
    }
    rbipc_ring_signal_eventfd(ring);

    return RBIPC_OK;
}

RBIPC_LEAF
int rbipc_sub_create_ex(rbipc_ring_t *ring, bool from_oldest, rbipc_sub_t **out_sub) {
    if (RBIPC_UNLIKELY(!ring || !ring->hdr || !out_sub)) {
        return RBIPC_ERR_INVAL;
    }
    *out_sub = NULL;

    rbipc_sub_t *sub = (rbipc_sub_t *)malloc(sizeof(rbipc_sub_t));
    if (RBIPC_UNLIKELY(!sub)) {
        return RBIPC_ERR_NOMEM;
    }

    sub->ring = ring;
    uint64_t head = atomic_load_explicit(&ring->hdr->write_ticket, memory_order_acquire);
    uint32_t cap = ring->hdr->capacity;

    if (from_oldest) {
        sub->cursor = (head > cap) ? (head - cap) : 0;
    } else {
        sub->cursor = head;
    }

    *out_sub = sub;
    return RBIPC_OK;
}

RBIPC_LEAF
int rbipc_sub_create(rbipc_ring_t *ring, rbipc_sub_t **out_sub) {
    return rbipc_sub_create_ex(ring, true, out_sub);
}

RBIPC_LEAF
void rbipc_sub_destroy(rbipc_sub_t *sub) {
    if (sub) {
        free(sub);
    }
}

RBIPC_LEAF
int rbipc_sub_poll(rbipc_sub_t *sub, const void **out_buf, uint32_t *out_len, uint64_t *out_seq) {
    if (RBIPC_UNLIKELY(!sub || !sub->ring || !out_buf || !out_len || !out_seq)) {
        return RBIPC_ERR_INVAL;
    }

    rbipc_ring_t *ring = sub->ring;
    rbipc_shm_header_t *hdr = ring->hdr;
    uint64_t head = atomic_load_explicit(&hdr->write_ticket, memory_order_acquire);

    if (sub->cursor >= head) {
        return RBIPC_ERR_EMPTY;
    }

    uint32_t cap = hdr->capacity;
    if (head > cap && sub->cursor < (head - cap)) {
        sub->cursor = head - cap;
    }

    uint32_t mask = hdr->capacity_mask;
    const rbipc_slot_t *slots = (const rbipc_slot_t *)RBIPC_ASSUME_ALIGNED(ring->slots, RBIPC_CACHE_LINE);
    const rbipc_slot_t *slot = &slots[sub->cursor & mask];
    uint64_t seq = atomic_load_explicit(&slot->sequence, memory_order_acquire);

    if (rbipc_slot_is_ready(seq, sub->cursor)) {
        *out_buf = (const char *)ring->data_map + ((size_t)(sub->cursor & mask) * hdr->slot_size);
        *out_len = atomic_load_explicit(&slot->len, memory_order_acquire);
        *out_seq = sub->cursor;
        sub->cursor++;
        return RBIPC_OK;
    }

    return RBIPC_ERR_EMPTY;
}

RBIPC_LEAF
int rbipc_sub_read(rbipc_sub_t *sub, uint64_t timeout_ns, const void **out_buf, uint32_t *out_len, uint64_t *out_seq) {
    if (RBIPC_UNLIKELY(!sub || !sub->ring)) {
        return RBIPC_ERR_INVAL;
    }

    rbipc_ring_t *ring = sub->ring;
    rbipc_shm_header_t *hdr = ring->hdr;
    rbipc_sync_state_t sync_state;
    rbipc_sync_state_init(&sync_state, timeout_ns);

    while (true) {
        int rc = rbipc_sub_poll(sub, out_buf, out_len, out_seq);
        if (rc == RBIPC_OK) {
            return RBIPC_OK;
        }

        if (RBIPC_UNLIKELY(rbipc_ring_is_shutdown(hdr))) {
            return RBIPC_ERR_SHUTDOWN;
        }

        if (timeout_ns == 0) {
            return RBIPC_ERR_EMPTY;
        }

        rbipc_sync_register_waiter(&hdr->futex_waiters);
        rc = rbipc_sub_poll(sub, out_buf, out_len, out_seq);
        if (rc == RBIPC_OK) {
            rbipc_sync_deregister_waiter(&hdr->futex_waiters);
            return RBIPC_OK;
        }

        int wait_rc = rbipc_sync_wait_registered(&sync_state, &hdr->futex_seq, &hdr->futex_waiters);
        if (wait_rc != 0) {
            return wait_rc;
        }
    }
}

RBIPC_LEAF
uint64_t rbipc_sub_get_lag(const rbipc_sub_t *sub) {
    if (RBIPC_UNLIKELY(!sub || !sub->ring || !sub->ring->hdr)) {
        return 0;
    }
    uint64_t head = atomic_load_explicit(&sub->ring->hdr->write_ticket, memory_order_acquire);
    return (head > sub->cursor) ? (head - sub->cursor) : 0;
}
