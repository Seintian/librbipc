/**
 * @file rbipc_audit.c
 * @brief Implementation of peer auditing, crash recovery, and lease
 * reclamation.
 *
 * @details Implements process liveness auditing, dead producer crash poisoning,
 * dead consumer recycling, orphaned ticket-hole healing, and consumer lease
 * timeouts.
 *
 * @author Seintian
 * @date 2026
 * @copyright Apache License 2.0
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "rbipc_audit.h"
#include "rbipc_arch.h"
#include "rbipc_predicate.h"
#include "rbipc_slot.h"
#include "rbipc_sync.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef SYS_pidfd_open
#if defined(__x86_64__)
#define SYS_pidfd_open 434
#elif defined(__aarch64__)
#define SYS_pidfd_open 434
#elif defined(__i386__)
#define SYS_pidfd_open 434
#endif
#endif

RBIPC_LEAF
bool rbipc_audit_is_peer_alive(pid_t pid) {
  if (RBIPC_UNLIKELY(pid <= 0))
    return false;

#ifdef SYS_pidfd_open
  /* Modern Linux 5.3+ process descriptor auditing to eliminate PID recycling
   * races */
  int pfd = (int)syscall(SYS_pidfd_open, pid, 0);
  if (pfd >= 0) {
    struct pollfd fds = {.fd = pfd, .events = POLLIN, .revents = 0};
    int ret = poll(&fds, 1, 0);
    close(pfd);
    uint32_t revents = (uint32_t)(uint16_t)fds.revents;
    uint32_t poll_mask =
        (uint32_t)POLLIN | (uint32_t)POLLHUP | (uint32_t)POLLERR;
    if (ret > 0 && (revents & poll_mask) != 0U) {
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
bool rbipc_audit_recover_dead_producer(rbipc_shm_header_t *hdr,
                                       rbipc_slot_t *slot, uint64_t ticket) {
  if (RBIPC_UNLIKELY(!hdr || !slot))
    return false;
  uint32_t state = atomic_load_explicit(&slot->state, memory_order_acquire);
  if (state == RBIPC_SLOT_RESERVED || state == RBIPC_SLOT_RESERVING) {
    pid_t ppid =
        (pid_t)atomic_load_explicit(&slot->producer_pid, memory_order_relaxed);
    if (ppid > 0 && !rbipc_audit_is_peer_alive(ppid)) {
      if (rbipc_slot_poison(slot, ticket)) {
        atomic_fetch_add_explicit(&hdr->poisoned_slots_count, 1,
                                  memory_order_relaxed);
        atomic_fetch_add_explicit(&hdr->dead_producers_recovered, 1,
                                  memory_order_relaxed);
        atomic_thread_fence(memory_order_seq_cst);
        rbipc_sync_wake_all(&hdr->futex_seq, &hdr->futex_waiters);
        return true;
      }
    }
  }
  return false;
}

RBIPC_LEAF
void rbipc_audit_recover_dead_producer_range(rbipc_shm_header_t *hdr,
                                             rbipc_slot_t *slots, uint64_t t,
                                             uint32_t count) {
  if (RBIPC_UNLIKELY(!hdr || !slots))
    return;
  const uint32_t mask = hdr->capacity_mask;
  for (uint32_t i = 0; i < count; ++i) {
    uint64_t ticket_i = t + (uint64_t)i;
    rbipc_slot_t *slot_i = &slots[ticket_i & mask];
    uint64_t seq =
        atomic_load_explicit(&slot_i->sequence, memory_order_acquire);
    if (rbipc_slot_is_ready(seq, ticket_i)) {
      continue;
    }
    rbipc_audit_recover_dead_producer(hdr, slot_i, ticket_i);
  }
}

RBIPC_LEAF
bool rbipc_audit_recover_dead_consumer(rbipc_shm_header_t *hdr,
                                       rbipc_slot_t *slot, uint64_t ticket) {
  if (RBIPC_UNLIKELY(!hdr || !slot))
    return false;
  uint64_t lease_ns =
      atomic_load_explicit(&hdr->lease_timeout_ns, memory_order_relaxed);
  if (lease_ns > 0) {
    uint64_t now_ns = rbipc_clock_monotonic_ns();
    if (rbipc_slot_recover_expired_lease(slot, ticket, lease_ns, now_ns)) {
      atomic_fetch_add_explicit(&hdr->leases_revoked_count, 1,
                                memory_order_relaxed);
      atomic_thread_fence(memory_order_seq_cst);
      rbipc_sync_wake_all(&hdr->write_futex_seq, &hdr->write_waiters);
      return true;
    }
  }
  if (rbipc_slot_recover_dead_consumer(slot, ticket)) {
    atomic_fetch_add_explicit(&hdr->dead_consumers_recovered, 1,
                              memory_order_relaxed);
    atomic_thread_fence(memory_order_seq_cst);
    rbipc_sync_wake_all(&hdr->write_futex_seq, &hdr->write_waiters);
    return true;
  }
  return false;
}

RBIPC_LEAF
void rbipc_audit_recover_dead_consumer_range(rbipc_shm_header_t *hdr,
                                             rbipc_slot_t *slots, uint64_t t,
                                             uint32_t count) {
  if (RBIPC_UNLIKELY(!hdr || !slots))
    return;
  const uint32_t mask = hdr->capacity_mask;
  for (uint32_t i = 0; i < count; ++i) {
    uint64_t ticket_i = t + (uint64_t)i;
    rbipc_slot_t *slot_i = &slots[ticket_i & mask];
    uint64_t seq =
        atomic_load_explicit(&slot_i->sequence, memory_order_acquire);
    if (rbipc_slot_is_vacant(seq, ticket_i)) {
      continue;
    }
    rbipc_audit_recover_dead_consumer(hdr, slot_i, ticket_i);
  }
}

RBIPC_LEAF
bool rbipc_audit_check_and_heal_ticket_hole(rbipc_shm_header_t *hdr,
                                            rbipc_slot_t *slot, uint64_t ticket,
                                            rbipc_sync_state_t *sync_state) {
  if (RBIPC_UNLIKELY(!hdr || !slot || !sync_state))
    return false;

  uint32_t state = atomic_load_explicit(&slot->state, memory_order_acquire);
  if (state == RBIPC_SLOT_EMPTY) {
    uint64_t wt =
        atomic_load_explicit(&hdr->write_ticket, memory_order_acquire);
    if (rbipc_seq_diff(wt, ticket) > 0) {
      uint64_t now_ts = rbipc_clock_monotonic_ns();
      if (atomic_load_explicit(&hdr->active_producers, memory_order_acquire) ==
              0 ||
          (sync_state->hole_first_seen_ns != 0 &&
           now_ts - sync_state->hole_first_seen_ns >=
               RBIPC_TICKET_HOLE_GRACE_NS)) {
        if (rbipc_slot_poison(slot, ticket)) {
          atomic_fetch_add_explicit(&hdr->poisoned_slots_count, 1,
                                    memory_order_relaxed);
          atomic_fetch_add_explicit(&hdr->ticket_holes_healed, 1,
                                    memory_order_relaxed);
          atomic_thread_fence(memory_order_seq_cst);
          rbipc_sync_wake_all(&hdr->futex_seq, &hdr->futex_waiters);
          return true;
        }
      } else if (sync_state->hole_first_seen_ns == 0) {
        sync_state->hole_first_seen_ns = now_ts;
      }
    }
  } else {
    sync_state->hole_first_seen_ns = 0;
  }
  return false;
}

RBIPC_LEAF
int rbipc_audit_revoke_expired_leases(rbipc_shm_header_t *hdr,
                                      rbipc_slot_t *slots) {
  if (RBIPC_UNLIKELY(!hdr || !slots))
    return RBIPC_ERR_INVAL;

  uint64_t lease =
      atomic_load_explicit(&hdr->lease_timeout_ns, memory_order_relaxed);
  if (lease == 0)
    return 0;

  uint64_t now = rbipc_clock_monotonic_ns();
  uint32_t cap = hdr->capacity;
  int revoked_count = 0;

  for (uint32_t i = 0; i < cap; ++i) {
    rbipc_slot_t *slot = &slots[i];
    uint32_t cpid =
        atomic_load_explicit(&slot->consumer_pid, memory_order_relaxed);
    if (cpid > 0) {
      uint64_t acquired_ts = atomic_load_explicit(&slot->acquire_timestamp_ns,
                                                  memory_order_relaxed);
      if (acquired_ts > 0 && now > acquired_ts &&
          (now - acquired_ts) >= lease) {
        uint64_t seq =
            atomic_load_explicit(&slot->sequence, memory_order_acquire);
        uint64_t target_ticket = (seq > 0 ? (seq - 1) : 0) + (uint64_t)cap;
        if (rbipc_slot_recover_expired_lease(slot, target_ticket, lease, now)) {
          revoked_count++;
        }
      }
    }
  }

  if (revoked_count > 0) {
    atomic_fetch_add_explicit(&hdr->leases_revoked_count,
                              (uint64_t)revoked_count, memory_order_relaxed);
    atomic_thread_fence(memory_order_seq_cst);
    rbipc_sync_wake_all(&hdr->write_futex_seq, &hdr->write_waiters);
  }
  return revoked_count;
}
