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

    char *data_base = (char *)RBIPC_ASSUME_ALIGNED(ring->data_map, RBIPC_CACHE_LINE);
    char *slot_ptr = data_base + ((size_t)(ticket & mask) * hdr->slot_size);
    if (len >= 256) {
        rbipc_stream_copy(slot_ptr, buf, len);
    } else {
        memcpy(slot_ptr, buf, len);
    }

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
int rbipc_publish_batch(rbipc_ring_t *ring, uint32_t count, const void * const *bufs, const uint32_t *lens) {
    if (RBIPC_UNLIKELY(!ring || !ring->hdr || !bufs || !lens || count == 0)) {
        return RBIPC_ERR_INVAL;
    }
    rbipc_shm_header_t *hdr = ring->hdr;
    for (uint32_t i = 0; i < count; ++i) {
        if (RBIPC_UNLIKELY(!bufs[i] || lens[i] > hdr->slot_size)) {
            return RBIPC_ERR_INVAL;
        }
    }

    uint32_t mask = hdr->capacity_mask;
    rbipc_slot_t *slots = (rbipc_slot_t *)RBIPC_ASSUME_ALIGNED(ring->slots, RBIPC_CACHE_LINE);
    char *data_base = (char *)RBIPC_ASSUME_ALIGNED(ring->data_map, RBIPC_CACHE_LINE);
    uint32_t slot_size = hdr->slot_size;
    pid_t my_pid = ring->cached_pid;

    uint64_t base_ticket = atomic_fetch_add_explicit(&hdr->write_ticket, (uint64_t)count, memory_order_relaxed);

    for (uint32_t i = 0; i < count; ++i) {
        uint64_t ticket_i = base_ticket + (uint64_t)i;
        rbipc_slot_t *slot_i = &slots[ticket_i & mask];

        atomic_store_explicit(&slot_i->producer_pid, (uint32_t)my_pid, memory_order_relaxed);
        atomic_store_explicit(&slot_i->consumer_pid, 0, memory_order_relaxed);
        atomic_store_explicit(&slot_i->acquire_timestamp_ns, 0, memory_order_relaxed);
        atomic_store_explicit(&slot_i->state, RBIPC_SLOT_RESERVED, memory_order_relaxed);

        char *slot_ptr = data_base + ((size_t)(ticket_i & mask) * slot_size);
        uint32_t len_i = lens[i];
        if (len_i >= 256) {
            rbipc_stream_copy(slot_ptr, bufs[i], len_i);
        } else {
            memcpy(slot_ptr, bufs[i], len_i);
        }

        atomic_store_explicit(&slot_i->len, len_i, memory_order_relaxed);
        atomic_store_explicit(&slot_i->state, RBIPC_SLOT_COMMITTED, memory_order_release);
        atomic_store_explicit(&slot_i->sequence, ticket_i + 1, memory_order_release);
    }

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
        const char *data_base = (const char *)RBIPC_ASSUME_ALIGNED(ring->data_map, RBIPC_CACHE_LINE);
        *out_buf = data_base + ((size_t)(sub->cursor & mask) * hdr->slot_size);
        *out_len = atomic_load_explicit(&slot->len, memory_order_acquire);
        *out_seq = sub->cursor;
        sub->cursor++;
        return RBIPC_OK;
    }

    return RBIPC_ERR_EMPTY;
}

RBIPC_LEAF
int rbipc_sub_poll_batch(rbipc_sub_t *sub, uint32_t max_count,
                         const void **out_bufs, uint32_t *out_lens,
                         uint64_t *out_seqs, uint32_t *out_polled) {
    if (RBIPC_UNLIKELY(!sub || !sub->ring || !out_bufs || !out_lens || !out_seqs || !out_polled || max_count == 0)) {
        return RBIPC_ERR_INVAL;
    }
    *out_polled = 0;

    rbipc_ring_t *ring = sub->ring;
    rbipc_shm_header_t *hdr = ring->hdr;
    uint64_t head = atomic_load_explicit(&hdr->write_ticket, memory_order_acquire);

    if (sub->cursor >= head) {
        return RBIPC_OK;
    }

    uint32_t cap = hdr->capacity;
    if (head > cap && sub->cursor < (head - cap)) {
        sub->cursor = head - cap;
    }

    uint32_t mask = hdr->capacity_mask;
    const rbipc_slot_t *slots = (const rbipc_slot_t *)RBIPC_ASSUME_ALIGNED(ring->slots, RBIPC_CACHE_LINE);
    const char *data_base = (const char *)RBIPC_ASSUME_ALIGNED(ring->data_map, RBIPC_CACHE_LINE);
    uint32_t slot_size = hdr->slot_size;
    uint32_t count = 0;

    while (count < max_count && sub->cursor < head) {
        uint64_t cur = sub->cursor;
        const rbipc_slot_t *slot = &slots[cur & mask];
        uint64_t seq = atomic_load_explicit(&slot->sequence, memory_order_acquire);
        if (!rbipc_slot_is_ready(seq, cur)) {
            break;
        }

        out_bufs[count] = data_base + ((size_t)(cur & mask) * slot_size);
        out_lens[count] = atomic_load_explicit(&slot->len, memory_order_acquire);
        out_seqs[count] = cur;
        sub->cursor++;
        count++;
    }

    *out_polled = count;
    return RBIPC_OK;
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
