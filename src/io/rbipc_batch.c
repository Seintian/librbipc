/**
 * @file rbipc_batch.c
 * @brief Implementation of B-Queue multi-message vector batching operations.
 *
 * @details Implements contiguous multi-slot reservations, commits,
 * acquisitions, and releases using SIMD vacancy/readiness scanning, dead-peer
 * auditing, and proportional futex dispatching.
 *
 * @author Seintian
 * @date 2026
 * @copyright Apache License 2.0
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "rbipc_batch.h"
#include "rbipc_io.h"
#include "rbipc_attr.h"
#include "rbipc_audit.h"
#include "rbipc_internal.h"
#include "rbipc_predicate.h"
#include "rbipc_reactor.h"
#include "rbipc_simd.h"
#include "rbipc_slot.h"
#include "rbipc_sync.h"

/**
 * @brief Populate reserved iovec descriptors and initialize slot ownership.
 */
static void
rbipc_batch_populate_reserved(rbipc_ring_t *RBIPC_RESTRICT ring, uint64_t t,
                              uint32_t count,
                              rbipc_iovec_t *RBIPC_RESTRICT iovecs) {
  const uint32_t mask = ring->cached_capacity_mask;
  const uint32_t slot_size = ring->cached_slot_size;
  rbipc_slot_t *slots =
      (rbipc_slot_t *)RBIPC_ASSUME_ALIGNED(ring->slots, RBIPC_CACHE_LINE);
  pid_t my_pid = ring->cached_pid;

  for (uint32_t i = 0; i < count; ++i) {
    uint64_t ticket_i = t + (uint64_t)i;
    uint32_t idx = (uint32_t)(ticket_i & mask);
    rbipc_slot_t *slot = &slots[idx];

    rbipc_slot_mark_reserved(slot, my_pid);

    iovecs[i].buf = rbipc_io_calc_slot_ptr(ring, ticket_i);
    iovecs[i].ticket = ticket_i;
    iovecs[i].max_len = slot_size;

    RBIPC_PREFETCH(iovecs[i].buf, RBIPC_PREFETCH_WRITE,
                   RBIPC_PREFETCH_LOCALITY_HIGH);
  }
  RBIPC_PREFETCH(&slots[(t + count) & mask], RBIPC_PREFETCH_WRITE,
                 RBIPC_PREFETCH_LOCALITY_HIGH);
}

/**
 * @brief Populate acquired rovec descriptors and stamp consumer ownership.
 */
static void
rbipc_batch_populate_acquired(const rbipc_ring_t *RBIPC_RESTRICT ring,
                              uint64_t t, uint32_t count,
                              rbipc_rovec_t *RBIPC_RESTRICT rovecs) {
  const uint32_t mask = ring->cached_capacity_mask;
  rbipc_slot_t *slots =
      (rbipc_slot_t *)RBIPC_ASSUME_ALIGNED(ring->slots, RBIPC_CACHE_LINE);
  pid_t my_pid = ring->cached_pid;
  uint64_t now_ts = rbipc_clock_monotonic_ns();

  for (uint32_t i = 0; i < count; ++i) {
    uint64_t ticket_i = t + (uint64_t)i;
    uint32_t idx = (uint32_t)(ticket_i & mask);
    rbipc_slot_t *slot = &slots[idx];
    uint32_t state = atomic_load_explicit(&slot->state, memory_order_acquire);

    rovecs[i].ticket = ticket_i;
    if (rbipc_slot_state_is_committed(state)) {
      rbipc_slot_mark_acquired(slot, my_pid, now_ts);
      rovecs[i].buf = rbipc_io_calc_slot_ptr(ring, ticket_i);
      rovecs[i].len = atomic_load_explicit(&slot->len, memory_order_acquire);
      RBIPC_PREFETCH(rovecs[i].buf, RBIPC_PREFETCH_READ,
                     RBIPC_PREFETCH_LOCALITY_HIGH);
    } else {
      rbipc_slot_mark_acquired(slot, my_pid, now_ts);
      rovecs[i].buf = NULL;
      rovecs[i].len = 0;
    }
  }
  RBIPC_PREFETCH(&slots[(t + count) & mask], RBIPC_PREFETCH_READ,
                 RBIPC_PREFETCH_LOCALITY_HIGH);
}

/**
 * @brief Validate that all written lengths do not exceed maximum slot capacity.
 */
static bool rbipc_batch_validate_lengths(uint32_t max_len, uint32_t count,
                                         const uint32_t *RBIPC_RESTRICT lens) {
  for (uint32_t i = 0; i < count; ++i) {
    if (RBIPC_UNLIKELY(lens[i] > max_len)) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Commit written payloads across a batch of slots.
 */
static void
rbipc_batch_commit_slots(rbipc_ring_t *RBIPC_RESTRICT ring, uint32_t count,
                         const uint64_t *RBIPC_RESTRICT tickets,
                         const uint32_t *RBIPC_RESTRICT written_lens) {
  const uint32_t mask = ring->cached_capacity_mask;
  rbipc_slot_t *slots =
      (rbipc_slot_t *)RBIPC_ASSUME_ALIGNED(ring->slots, RBIPC_CACHE_LINE);

  for (uint32_t i = 0; i < count; ++i) {
    uint64_t ticket = tickets[i];
    rbipc_slot_t *slot = &slots[ticket & mask];
    rbipc_slot_commit(slot, ticket, written_lens[i]);
  }
}

/**
 * @brief Release consumed read slots across a batch.
 */
static int rbipc_batch_release_slots(rbipc_ring_t *RBIPC_RESTRICT ring,
                                     uint32_t count,
                                     const uint64_t *RBIPC_RESTRICT tickets) {
  const uint32_t mask = ring->cached_capacity_mask;
  const uint32_t cap = ring->cached_capacity;
  rbipc_slot_t *slots =
      (rbipc_slot_t *)RBIPC_ASSUME_ALIGNED(ring->slots, RBIPC_CACHE_LINE);
  pid_t my_pid = ring->cached_pid;
  int rc = RBIPC_OK;

  for (uint32_t i = 0; i < count; ++i) {
    uint64_t ticket = tickets[i];
    rbipc_slot_t *slot = &slots[ticket & mask];
    if (RBIPC_UNLIKELY(!rbipc_slot_release_ex(slot, ticket, cap, my_pid))) {
      rc = RBIPC_ERR_TIMEOUT;
    }
  }
  return rc;
}

RBIPC_LEAF
int rbipc_reserve_write_batch_timeout(rbipc_ring_t *RBIPC_RESTRICT ring,
                                      uint32_t count, uint64_t timeout_ns,
                                      rbipc_iovec_t *RBIPC_RESTRICT iovecs,
                                      uint32_t *RBIPC_RESTRICT out_reserved) {
  if (RBIPC_UNLIKELY(!ring || !ring->hdr || !iovecs || !out_reserved ||
                     count == 0)) {
    return RBIPC_ERR_INVAL;
  }

  rbipc_shm_header_t *hdr = ring->hdr;
  if (count > hdr->capacity) {
    count = hdr->capacity;
  }

  uint64_t t = atomic_load_explicit(&hdr->write_ticket, memory_order_relaxed);

  rbipc_sync_state_t sync_state;
  rbipc_sync_state_init(&sync_state, timeout_ns);

  rbipc_slot_t *slots =
      (rbipc_slot_t *)RBIPC_ASSUME_ALIGNED(ring->slots, RBIPC_CACHE_LINE);

  for (;;) {
    if (RBIPC_UNLIKELY(rbipc_ring_is_shutdown(hdr))) {
      return RBIPC_ERR_SHUTDOWN;
    }

    rbipc_audit_recover_dead_consumer_range(hdr, slots, t, count);
    uint32_t avail =
        rbipc_simd_scan_vacant_slots(slots, ring->cached_capacity_mask, t, count);
    if (avail > 0) {
      if (rbipc_io_try_claim_write_ticket(hdr, &t, avail)) {
        rbipc_batch_populate_reserved(ring, t, avail, iovecs);
        *out_reserved = avail;
        return RBIPC_OK;
      }
      if (sync_state.has_deadline &&
          RBIPC_UNLIKELY(rbipc_clock_monotonic_ns() >= sync_state.deadline_ns)) {
        return (timeout_ns == 0) ? RBIPC_ERR_FULL : RBIPC_ERR_TIMEOUT;
      }
      rbipc_cpu_pause();
    } else {
      if (timeout_ns == 0) {
        return RBIPC_ERR_FULL;
      }
      if (ring && (ring->wait_policy & RBIPC_WAIT_ADAPTIVE)) {
        for (uint32_t spin = 0; spin < RBIPC_SPIN_COUNT_DEFAULT; ++spin) {
          rbipc_cpu_pause();
          avail =
              rbipc_simd_scan_vacant_slots(slots, ring->cached_capacity_mask, t, count);
          if (avail > 0 || RBIPC_UNLIKELY(rbipc_ring_is_shutdown(hdr))) {
            break;
          }
        }
        if (avail > 0) {
          continue;
        }
      }
      rbipc_sync_register_waiter(&hdr->write_waiters);
      rbipc_audit_recover_dead_consumer_range(hdr, slots, t, count);
      avail = rbipc_simd_scan_vacant_slots(slots, ring->cached_capacity_mask, t, count);
      if (avail > 0 || RBIPC_UNLIKELY(rbipc_ring_is_shutdown(hdr))) {
        rbipc_sync_deregister_waiter(&hdr->write_waiters);
        continue;
      }
      int rc = rbipc_sync_wait_registered_or_shutdown(
          &sync_state, &hdr->write_futex_seq, &hdr->write_waiters,
          &hdr->shutdown_flag);
      if (RBIPC_UNLIKELY(rc != 0)) {
        return rc;
      }
      t = atomic_load_explicit(&hdr->write_ticket, memory_order_relaxed);
    }
  }
}

RBIPC_LEAF
int rbipc_reserve_write_batch(rbipc_ring_t *RBIPC_RESTRICT ring, uint32_t count,
                              rbipc_iovec_t *RBIPC_RESTRICT iovecs,
                              uint32_t *RBIPC_RESTRICT out_reserved) {
  return rbipc_reserve_write_batch_timeout(ring, count, UINT64_MAX, iovecs,
                                           out_reserved);
}

RBIPC_LEAF
int rbipc_reserve_write_batch_nonblock(rbipc_ring_t *RBIPC_RESTRICT ring,
                                       uint32_t count,
                                       rbipc_iovec_t *RBIPC_RESTRICT iovecs,
                                       uint32_t *RBIPC_RESTRICT out_reserved) {
  return rbipc_reserve_write_batch_timeout(ring, count, 0, iovecs,
                                           out_reserved);
}

RBIPC_LEAF
int rbipc_commit_write_batch(rbipc_ring_t *RBIPC_RESTRICT ring, uint32_t count,
                             const uint64_t *RBIPC_RESTRICT tickets,
                             const uint32_t *RBIPC_RESTRICT written_lens) {
  if (RBIPC_UNLIKELY(!ring || !ring->hdr || !tickets || !written_lens ||
                     count == 0)) {
    return RBIPC_ERR_INVAL;
  }

  rbipc_shm_header_t *hdr = ring->hdr;
  if (RBIPC_UNLIKELY(
          !rbipc_batch_validate_lengths(hdr->slot_size, count, written_lens))) {
    return RBIPC_ERR_INVAL;
  }

  rbipc_batch_commit_slots(ring, count, tickets, written_lens);

  atomic_signal_fence(memory_order_seq_cst);
  if (RBIPC_UNLIKELY(rbipc_sync_has_waiters(&hdr->futex_waiters))) {
    rbipc_sync_wake_n(&hdr->futex_seq, &hdr->futex_waiters, count);
  }
  rbipc_reactor_signal(ring);

  return RBIPC_OK;
}

RBIPC_LEAF
int rbipc_read_acquire_batch_timeout(rbipc_ring_t *RBIPC_RESTRICT ring,
                                     uint32_t count, uint64_t timeout_ns,
                                     rbipc_rovec_t *RBIPC_RESTRICT rovecs,
                                     uint32_t *RBIPC_RESTRICT out_acquired) {
  if (RBIPC_UNLIKELY(!ring || !ring->hdr || !rovecs || !out_acquired ||
                     count == 0)) {
    return RBIPC_ERR_INVAL;
  }

  rbipc_shm_header_t *hdr = ring->hdr;
  if (count > hdr->capacity) {
    count = hdr->capacity;
  }

  uint64_t t = atomic_load_explicit(&hdr->read_ticket, memory_order_relaxed);

  rbipc_sync_state_t sync_state;
  rbipc_sync_state_init(&sync_state, timeout_ns);

  rbipc_slot_t *slots =
      (rbipc_slot_t *)RBIPC_ASSUME_ALIGNED(ring->slots, RBIPC_CACHE_LINE);

  for (;;) {
    rbipc_audit_recover_dead_producer_range(hdr, slots, t, count);
    uint32_t avail =
        rbipc_simd_scan_ready_slots(slots, ring->cached_capacity_mask, t, count);
    if (avail > 0) {
      if (rbipc_io_try_claim_read_ticket(hdr, &t, avail)) {
        rbipc_batch_populate_acquired(ring, t, avail, rovecs);
        *out_acquired = avail;
        return RBIPC_OK;
      }
      if (sync_state.has_deadline &&
          RBIPC_UNLIKELY(rbipc_clock_monotonic_ns() >= sync_state.deadline_ns)) {
        return (timeout_ns == 0) ? RBIPC_ERR_EMPTY : RBIPC_ERR_TIMEOUT;
      }
      rbipc_cpu_pause();
    } else {
      if (RBIPC_UNLIKELY(rbipc_ring_is_drained_on_shutdown(hdr, t))) {
        return RBIPC_ERR_SHUTDOWN;
      }
      if (timeout_ns == 0) {
        return RBIPC_ERR_EMPTY;
      }
      if (ring && (ring->wait_policy & RBIPC_WAIT_ADAPTIVE)) {
        for (uint32_t spin = 0; spin < RBIPC_SPIN_COUNT_DEFAULT; ++spin) {
          rbipc_cpu_pause();
          avail =
              rbipc_simd_scan_ready_slots(slots, ring->cached_capacity_mask, t, count);
          if (avail > 0 ||
              RBIPC_UNLIKELY(rbipc_ring_is_drained_on_shutdown(hdr, t))) {
            break;
          }
        }
        if (avail > 0) {
          continue;
        }
      }

      rbipc_sync_register_waiter(&hdr->futex_waiters);
      rbipc_audit_recover_dead_producer_range(hdr, slots, t, count);
      avail = rbipc_simd_scan_ready_slots(slots, ring->cached_capacity_mask, t, count);
      if (avail > 0 ||
          RBIPC_UNLIKELY(rbipc_ring_is_drained_on_shutdown(hdr, t))) {
        rbipc_sync_deregister_waiter(&hdr->futex_waiters);
        continue;
      }
      int rc = rbipc_sync_wait_registered_or_shutdown(
          &sync_state, &hdr->futex_seq, &hdr->futex_waiters,
          &hdr->shutdown_flag);
      if (RBIPC_UNLIKELY(rc != 0)) {
        return rc;
      }

      /* Unified ticket-hole audit & healing */
      rbipc_slot_t *slot_t = &slots[t & ring->cached_capacity_mask];
      rbipc_audit_check_and_heal_ticket_hole(hdr, slot_t, t, &sync_state);

      t = atomic_load_explicit(&hdr->read_ticket, memory_order_relaxed);
    }
  }
}

RBIPC_LEAF
int rbipc_read_acquire_batch(rbipc_ring_t *RBIPC_RESTRICT ring, uint32_t count,
                             rbipc_rovec_t *RBIPC_RESTRICT rovecs,
                             uint32_t *RBIPC_RESTRICT out_acquired) {
  return rbipc_read_acquire_batch_timeout(ring, count, UINT64_MAX, rovecs,
                                          out_acquired);
}

RBIPC_LEAF
int rbipc_read_acquire_batch_nonblock(rbipc_ring_t *RBIPC_RESTRICT ring,
                                      uint32_t count,
                                      rbipc_rovec_t *RBIPC_RESTRICT rovecs,
                                      uint32_t *RBIPC_RESTRICT out_acquired) {
  return rbipc_read_acquire_batch_timeout(ring, count, 0, rovecs, out_acquired);
}

RBIPC_LEAF
int rbipc_read_release_batch(rbipc_ring_t *RBIPC_RESTRICT ring, uint32_t count,
                             const uint64_t *RBIPC_RESTRICT tickets) {
  if (RBIPC_UNLIKELY(!ring || !ring->hdr || !tickets || count == 0)) {
    return RBIPC_ERR_INVAL;
  }

  rbipc_shm_header_t *hdr = ring->hdr;
  int rc = rbipc_batch_release_slots(ring, count, tickets);

  atomic_signal_fence(memory_order_seq_cst);
  if (RBIPC_UNLIKELY(rbipc_sync_has_waiters(&hdr->write_waiters))) {
    rbipc_sync_wake_n(&hdr->write_futex_seq, &hdr->write_waiters, count);
  }
  rbipc_reactor_signal(ring);

  return rc;
}
