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
#include <sys/syscall.h>
#include <poll.h>

#ifndef SYS_pidfd_open
#if defined(__x86_64__)
#define SYS_pidfd_open 434
#elif defined(__aarch64__)
#define SYS_pidfd_open 434
#elif defined(__i386__)
#define SYS_pidfd_open 434
#endif
#endif

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
    atomic_init(&slot->consumer_pid, 0);
    atomic_init(&slot->acquire_timestamp_ns, 0);
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

#ifdef SYS_pidfd_open
    /* Modern Linux 5.3+ process descriptor auditing to eliminate PID recycling races */
    int pfd = (int)syscall(SYS_pidfd_open, pid, 0);
    if (pfd >= 0) {
        struct pollfd fds = {
            .fd = pfd,
            .events = POLLIN,
            .revents = 0
        };
        int ret = poll(&fds, 1, 0);
        close(pfd);
        if (ret > 0 && (fds.revents & (POLLIN | POLLHUP | POLLERR))) {
            return false; /* Process has terminated / exited */
        }
        if (ret == 0) {
            return true; /* Process is alive and running */
        }
    } else if (errno == ESRCH) {
        return false; /* Process does not exist */
    }
#endif

    /* Graceful fallback for older kernels (<= 5.2) or restricted environments */
    if (kill(pid, 0) == 0) {
        return true;
    }
    return errno != ESRCH;
}

RBIPC_LEAF
void rbipc_slot_mark_reserving(rbipc_slot_t *slot, pid_t pid) {
    if (RBIPC_UNLIKELY(rbipc_is_null(slot))) return;
    atomic_store_explicit(&slot->producer_pid, (uint32_t)pid, memory_order_relaxed);
    atomic_store_explicit(&slot->state, RBIPC_SLOT_RESERVING, memory_order_release);
}

RBIPC_LEAF
void rbipc_slot_mark_reserved(rbipc_slot_t *slot, pid_t pid) {
    if (RBIPC_UNLIKELY(rbipc_is_null(slot))) return;
    atomic_store_explicit(&slot->producer_pid, (uint32_t)pid, memory_order_relaxed);
    atomic_store_explicit(&slot->state, RBIPC_SLOT_RESERVED, memory_order_release);
}

RBIPC_LEAF
void rbipc_slot_commit(rbipc_slot_t *slot, uint64_t ticket, uint32_t len) {
    if (RBIPC_UNLIKELY(rbipc_is_null(slot))) return;
    atomic_store_explicit(&slot->len, len, memory_order_relaxed);
    atomic_store_explicit(&slot->state, RBIPC_SLOT_COMMITTED, memory_order_release);
    /* Advance sequence to ticket + 1 with release semantics to make payload visible */
    atomic_store_explicit(&slot->sequence, ticket + 1, memory_order_release);
}

RBIPC_LEAF
bool rbipc_slot_poison(rbipc_slot_t *slot, uint64_t ticket) {
    if (RBIPC_UNLIKELY(rbipc_is_null(slot))) return false;
    uint32_t state = atomic_load_explicit(&slot->state, memory_order_acquire);
    while (state == RBIPC_SLOT_RESERVED || state == RBIPC_SLOT_RESERVING || state == RBIPC_SLOT_EMPTY) {
        if (atomic_compare_exchange_weak_explicit(&slot->state, &state,
                                                  RBIPC_SLOT_POISONED,
                                                  memory_order_release,
                                                  memory_order_relaxed)) {
            atomic_store_explicit(&slot->sequence, ticket + 1, memory_order_release);
            return true;
        }
    }
    return false;
}

RBIPC_LEAF
void rbipc_slot_mark_acquired(rbipc_slot_t *slot, pid_t pid, uint64_t timestamp_ns) {
    if (RBIPC_UNLIKELY(rbipc_is_null(slot))) return;
    atomic_store_explicit(&slot->consumer_pid, (uint32_t)pid, memory_order_relaxed);
    atomic_store_explicit(&slot->acquire_timestamp_ns, timestamp_ns, memory_order_relaxed);
}

RBIPC_LEAF
bool rbipc_slot_recover_dead_consumer(rbipc_slot_t *slot, uint64_t ticket) {
    if (RBIPC_UNLIKELY(rbipc_is_null(slot))) return false;
    uint32_t cpid = atomic_load_explicit(&slot->consumer_pid, memory_order_relaxed);
    if (cpid > 0 && !rbipc_slot_is_peer_alive((pid_t)cpid)) {
        atomic_store_explicit(&slot->consumer_pid, 0, memory_order_relaxed);
        atomic_store_explicit(&slot->producer_pid, 0, memory_order_relaxed);
        atomic_store_explicit(&slot->acquire_timestamp_ns, 0, memory_order_relaxed);
        atomic_store_explicit(&slot->len, 0, memory_order_relaxed);
        atomic_store_explicit(&slot->state, RBIPC_SLOT_EMPTY, memory_order_release);
        atomic_store_explicit(&slot->sequence, ticket, memory_order_release);
        return true;
    }
    return false;
}

RBIPC_LEAF
bool rbipc_slot_recover_expired_lease(rbipc_slot_t *slot, uint64_t ticket, uint32_t capacity, uint64_t lease_timeout_ns, uint64_t now_ns) {
    if (RBIPC_UNLIKELY(rbipc_is_null(slot) || lease_timeout_ns == 0)) return false;
    uint32_t cpid = atomic_load_explicit(&slot->consumer_pid, memory_order_relaxed);
    if (cpid > 0) {
        uint64_t acquired_ts = atomic_load_explicit(&slot->acquire_timestamp_ns, memory_order_relaxed);
        if (acquired_ts > 0 && now_ns > acquired_ts && (now_ns - acquired_ts) >= lease_timeout_ns) {
            atomic_store_explicit(&slot->consumer_pid, 0, memory_order_relaxed);
            atomic_store_explicit(&slot->producer_pid, 0, memory_order_relaxed);
            atomic_store_explicit(&slot->acquire_timestamp_ns, 0, memory_order_relaxed);
            atomic_store_explicit(&slot->len, 0, memory_order_relaxed);
            atomic_store_explicit(&slot->state, RBIPC_SLOT_EMPTY, memory_order_release);
            atomic_store_explicit(&slot->sequence, ticket + capacity, memory_order_release);
            return true;
        }
    }
    return false;
}

RBIPC_LEAF
bool rbipc_slot_release_ex(rbipc_slot_t *slot, uint64_t ticket, uint32_t capacity, pid_t expected_pid) {
    if (RBIPC_UNLIKELY(rbipc_is_null(slot))) return false;
    uint32_t cpid = (uint32_t)expected_pid;
    if (cpid > 0) {
        uint32_t current_cpid = cpid;
        if (!atomic_compare_exchange_strong_explicit(&slot->consumer_pid, &current_cpid, 0,
                                                     memory_order_acq_rel, memory_order_relaxed)) {
            /* Lease was revoked, expired, or held by another peer */
            return false;
        }
    } else {
        atomic_store_explicit(&slot->consumer_pid, 0, memory_order_relaxed);
    }
    atomic_store_explicit(&slot->acquire_timestamp_ns, 0, memory_order_relaxed);
    atomic_store_explicit(&slot->state, RBIPC_SLOT_EMPTY, memory_order_release);
    /* Advance slot sequence by capacity to allow the next cycle turn */
    atomic_store_explicit(&slot->sequence, ticket + capacity, memory_order_release);
    return true;
}
