/**
 * @file rbipc_slot.c
 * @brief Implementation of slot state transitions, sequence management, and crash detection.
 *
 * @details Implements atomic memory ordering synchronization for slot transitions,
 * sequence progression under modular ring turns, and POSIX process auditing.
 *
 * @author Christian Santarelli
 * @date 2026
 * @copyright Apache License 2.0
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "rbipc_slot.h"
#include "rbipc_attr.h"
#include "rbipc_predicate.h"

#include <signal.h>
#include <errno.h>
#include <unistd.h>

/**
 * @brief Initialize an individual slot control descriptor in shared memory.
 *
 * @param[out] slot  Direct pointer to slot descriptor.
 * @param[in]  index Linear index corresponding to initial sequence position.
 */
RBIPC_INLINE void rbipc_slot_init_single(rbipc_slot_t *slot, uint32_t index) {
    atomic_init(&slot->sequence, index);
    atomic_init(&slot->state, RBIPC_SLOT_EMPTY);
    atomic_init(&slot->producer_pid, 0);
    atomic_init(&slot->len, 0);
}

RBIPC_LEAF
void rbipc_slot_init_table(rbipc_slot_t * RBIPC_RESTRICT slots, uint32_t capacity) {
    if (RBIPC_UNLIKELY(rbipc_is_null(slots))) return;
    rbipc_slot_t *s = (rbipc_slot_t *)RBIPC_ASSUME_ALIGNED(slots, RBIPC_CACHE_LINE);
    for (uint32_t i = 0; i < capacity; ++i) {
        rbipc_slot_init_single(&s[i], i);
    }
}

RBIPC_LEAF
bool rbipc_slot_is_peer_alive(pid_t pid) {
    if (RBIPC_UNLIKELY(pid <= 0)) return false;
    if (kill(pid, 0) == 0) {
        return true;
    }
    return errno != ESRCH;
}

RBIPC_LEAF
void rbipc_slot_mark_reserved(rbipc_slot_t *slot, pid_t pid) {
    if (RBIPC_UNLIKELY(rbipc_is_null(slot))) return;
    atomic_store_explicit(&slot->producer_pid, (uint32_t)pid, memory_order_relaxed);
    atomic_store_explicit(&slot->state, RBIPC_SLOT_RESERVED, memory_order_release);
}

RBIPC_LEAF
void rbipc_slot_commit(rbipc_slot_t *slot, uint32_t ticket, uint32_t len) {
    if (RBIPC_UNLIKELY(rbipc_is_null(slot))) return;
    atomic_store_explicit(&slot->len, len, memory_order_relaxed);
    atomic_store_explicit(&slot->state, RBIPC_SLOT_COMMITTED, memory_order_release);
    /* Advance sequence to ticket + 1 with release semantics to make payload visible */
    atomic_store_explicit(&slot->sequence, ticket + 1, memory_order_release);
}

RBIPC_LEAF
bool rbipc_slot_poison(rbipc_slot_t *slot, uint32_t ticket) {
    if (RBIPC_UNLIKELY(rbipc_is_null(slot))) return false;
    uint32_t expected = RBIPC_SLOT_RESERVED;
    if (atomic_compare_exchange_strong_explicit(&slot->state, &expected,
                                                RBIPC_SLOT_POISONED,
                                                memory_order_release,
                                                memory_order_relaxed)) {
        atomic_store_explicit(&slot->sequence, ticket + 1, memory_order_release);
        return true;
    }
    return false;
}

RBIPC_LEAF
void rbipc_slot_release(rbipc_slot_t *slot, uint32_t ticket, uint32_t capacity) {
    if (RBIPC_UNLIKELY(rbipc_is_null(slot))) return;
    atomic_store_explicit(&slot->state, RBIPC_SLOT_EMPTY, memory_order_release);
    /* Advance slot sequence by capacity to allow the next cycle turn */
    atomic_store_explicit(&slot->sequence, ticket + capacity, memory_order_release);
}
