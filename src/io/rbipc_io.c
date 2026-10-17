/**
 * @file rbipc_io.c
 * @brief Lock-free, wait-free single-slot ring buffer I/O pipeline
 * implementation.
 *
 * @details Implements single-slot zero-copy write reservation, atomic
 * publication, read acquisition, and slot reclamation. Coordinates
 * synchronization via cache-line isolated sequence state machines and adaptive
 * futex backoff.
 *
 * References:
 * - Dmitry Vyukov (2010), "Bounded MPMC Queue", 1024cores.
 * - Ulrich Drepper (2007), "What Every Programmer Should Know About Memory".
 *
 * @author Seintian
 * @date 2026
 * @copyright Apache License 2.0
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "rbipc_io.h"
#include "rbipc_audit.h"
#include "rbipc_internal.h"
#include "rbipc_predicate.h"
#include "rbipc_reactor.h"
#include "rbipc_slot.h"
#include "rbipc_sync.h"
#include "rbipc_futex.h"

#include <string.h>

/* ============================================================================
 * Internal Fast-Path Ticket and Reservation Helpers
 * ============================================================================
 */


/**
 * @brief Finalize single-slot write reservation and populate return pointers.
 */
RBIPC_INLINE void
rbipc_io_complete_write_reservation(rbipc_ring_t *RBIPC_RESTRICT ring,
                                    rbipc_slot_t *RBIPC_RESTRICT slot,
                                    uint64_t t, void **RBIPC_RESTRICT out_buf,
                                    uint64_t *RBIPC_RESTRICT ticket) {
  rbipc_slot_mark_reserved(slot, ring->cached_pid);
  *out_buf = rbipc_io_calc_slot_ptr(ring, t);
  *ticket = t;
  rbipc_io_prefetch_write_next(ring, t + 1);
}

/**
 * @brief Finalize single-slot read acquisition and extract message payload
 * pointers.
 */
RBIPC_INLINE int rbipc_io_complete_read_acquisition(
    const rbipc_ring_t *RBIPC_RESTRICT ring, rbipc_slot_t *RBIPC_RESTRICT slot,
    uint64_t ticket, const void **RBIPC_RESTRICT out_buf,
    uint32_t *RBIPC_RESTRICT out_len, uint64_t *RBIPC_RESTRICT out_ticket) {
  *out_ticket = ticket;
  uint32_t state = atomic_load_explicit(&slot->state, memory_order_acquire);
  uint64_t lease_timeout = atomic_load_explicit(&ring->hdr->lease_timeout_ns, memory_order_relaxed);
  uint64_t acq_ts = RBIPC_UNLIKELY(lease_timeout > 0) ? rbipc_clock_monotonic_ns() : 0;

  if (RBIPC_LIKELY(rbipc_slot_state_is_committed(state))) {
    rbipc_slot_mark_acquired(slot, ring->cached_pid, acq_ts);
    *out_buf = rbipc_io_calc_slot_ptr(ring, ticket);
    *out_len = atomic_load_explicit(&slot->len, memory_order_acquire);
    rbipc_io_prefetch_read_next(ring, ticket + 1);
    return RBIPC_OK;
  }
  if (rbipc_slot_state_is_poisoned(state)) {
    rbipc_slot_mark_acquired(slot, ring->cached_pid, acq_ts);
    *out_buf = NULL;
    *out_len = 0;
    return RBIPC_ERR_POISONED;
  }

  /* Defensive bounded micro-pause if state observation lagged behind slot
   * sequence */
  for (uint32_t p = 0; p < RBIPC_SPIN_COUNT_DEFAULT; ++p) {
    rbipc_cpu_pause();
    state = atomic_load_explicit(&slot->state, memory_order_acquire);
    if (rbipc_slot_state_is_committed(state)) {
      rbipc_slot_mark_acquired(slot, ring->cached_pid, acq_ts);
      *out_buf = rbipc_io_calc_slot_ptr(ring, ticket);
      *out_len = atomic_load_explicit(&slot->len, memory_order_acquire);
      rbipc_io_prefetch_read_next(ring, ticket + 1);
      return RBIPC_OK;
    }
    if (rbipc_slot_state_is_poisoned(state)) {
      rbipc_slot_mark_acquired(slot, ring->cached_pid, acq_ts);
      *out_buf = NULL;
      *out_len = 0;
      return RBIPC_ERR_POISONED;
    }
  }
  return RBIPC_ERR_BUSY;
}

typedef struct {
  const rbipc_shm_header_t *hdr;
  const rbipc_slot_t *slot;
  uint64_t ticket;
} rbipc_io_slot_pred_ctx_t;

static bool rbipc_io_pred_slot_vacant(void *arg) {
  const rbipc_io_slot_pred_ctx_t *ctx = (const rbipc_io_slot_pred_ctx_t *)arg;
  uint64_t seq =
      atomic_load_explicit(&ctx->slot->sequence, memory_order_acquire);
  return rbipc_slot_is_vacant(seq, ctx->ticket) ||
         rbipc_ring_is_shutdown(ctx->hdr);
}

static bool rbipc_io_pred_slot_ready(void *arg) {
  const rbipc_io_slot_pred_ctx_t *ctx = (const rbipc_io_slot_pred_ctx_t *)arg;
  uint64_t seq =
      atomic_load_explicit(&ctx->slot->sequence, memory_order_acquire);
  return rbipc_slot_is_ready(seq, ctx->ticket) ||
         rbipc_ring_is_shutdown(ctx->hdr);
}

/**
 * @brief Executes adaptive user-space spin-wait loop.
 */
RBIPC_INLINE bool rbipc_io_spin_wait(rbipc_spin_pred_t pred, void *ctx) {
  for (uint32_t spin = 0; spin < RBIPC_SPIN_COUNT_DEFAULT; ++spin) {
    rbipc_cpu_pause();
    if (pred(ctx)) {
      return true;
    }
  }
  return false;
}

/**
 * @brief Executes hardware-assisted monitor wait (UMONITOR/UMWAIT or WFET).
 */
RBIPC_INLINE bool rbipc_io_hardware_wait(const rbipc_ring_t *ring,
                                         const _Atomic uint64_t *addr,
                                         rbipc_spin_pred_t pred, void *ctx) {
  if (ring && ((ring->wait_policy & RBIPC_WAIT_ADAPTIVE) ||
               (ring->wait_policy & RBIPC_WAIT_HW_MONITOR))) {
    if (rbipc_cpu_has_waitpkg() || rbipc_cpu_has_wfxt()) {
      uint32_t substate = (ring->wait_policy & RBIPC_WAIT_ADAPTIVE) ? 1 : 0;
      return rbipc_sync_hardware_wait_substate(addr, 5000ULL, pred, ctx,
                                               substate) == RBIPC_OK;
    }
  }
  return false;
}

/**
 * @brief Handle backoff progression and futex suspension when writer encounters
 * full ring.
 */
static int rbipc_io_handle_write_backoff(
    const rbipc_ring_t *ring, rbipc_sync_state_t *RBIPC_RESTRICT sync_state,
    rbipc_shm_header_t *hdr, rbipc_slot_t *slot, uint64_t t_ticket,
    uint64_t timeout_ns, uint64_t *RBIPC_RESTRICT t) {
  if (RBIPC_UNLIKELY(rbipc_ring_is_shutdown(hdr))) {
    return RBIPC_ERR_SHUTDOWN;
  }
  rbipc_audit_recover_dead_consumer(hdr, slot, t_ticket);
  uint64_t initial_seq =
      atomic_load_explicit(&slot->sequence, memory_order_acquire);
  if (RBIPC_LIKELY(rbipc_slot_is_vacant(initial_seq, t_ticket))) {
    *t = atomic_load_explicit(&hdr->write_ticket, memory_order_relaxed);
    return RBIPC_OK;
  }

  if (timeout_ns == 0) {
    return RBIPC_ERR_FULL;
  }

  rbipc_io_slot_pred_ctx_t ctx = {
      .hdr = hdr,
      .slot = slot,
      .ticket = t_ticket,
  };

  /* Phase 1: Micro-spin if adaptive wait policy is configured */
  if (ring && (ring->wait_policy & RBIPC_WAIT_ADAPTIVE)) {
    if (rbipc_io_spin_wait(rbipc_io_pred_slot_vacant, &ctx)) {
      *t = atomic_load_explicit(&hdr->write_ticket, memory_order_relaxed);
      return RBIPC_OK;
    }
  }

  /* Tier 1: Hardware-assisted monitor-wait (WAITPKG / WFET) */
  if (rbipc_io_hardware_wait(ring, &slot->sequence, rbipc_io_pred_slot_vacant, &ctx)) {
    *t = atomic_load_explicit(&hdr->write_ticket, memory_order_relaxed);
    return RBIPC_OK;
  }

  /* Phase 2: Passive Futex Sleep with Dekker-style double-check */
  rbipc_sync_register_waiter(&hdr->write_waiters);

  uint64_t seq = atomic_load_explicit(&slot->sequence, memory_order_acquire);
  if (RBIPC_LIKELY(rbipc_slot_is_vacant(seq, t_ticket))) {
    rbipc_sync_deregister_waiter(&hdr->write_waiters);
    *t = atomic_load_explicit(&hdr->write_ticket, memory_order_relaxed);
    return RBIPC_OK;
  }
  if (RBIPC_UNLIKELY(rbipc_ring_is_shutdown(hdr))) {
    rbipc_sync_deregister_waiter(&hdr->write_waiters);
    return RBIPC_ERR_SHUTDOWN;
  }

  int rc = rbipc_sync_wait_slot_registered(
      sync_state, &slot->sequence, seq, &hdr->write_futex_seq,
      &hdr->write_waiters, &hdr->shutdown_flag);
  if (RBIPC_UNLIKELY(rc != 0)) {
    return rc;
  }
  *t = atomic_load_explicit(&hdr->write_ticket, memory_order_relaxed);
  return RBIPC_OK;
}

/**
 * @brief Handle backoff progression and futex suspension when reader encounters
 * empty ring.
 */
static int rbipc_io_handle_read_backoff(
    const rbipc_ring_t *ring, rbipc_sync_state_t *RBIPC_RESTRICT sync_state,
    rbipc_shm_header_t *hdr, rbipc_slot_t *slot, uint64_t t,
    uint64_t timeout_ns, uint64_t *RBIPC_RESTRICT out_next_t) {
  rbipc_audit_recover_dead_producer(hdr, slot, t);

  if (RBIPC_UNLIKELY(rbipc_ring_is_drained_on_shutdown(hdr, t))) {
    return RBIPC_ERR_SHUTDOWN;
  }

  if (timeout_ns == 0) {
    return RBIPC_ERR_EMPTY;
  }

  rbipc_io_slot_pred_ctx_t ctx = {
      .hdr = hdr,
      .slot = slot,
      .ticket = t,
  };

  /* Phase 1: Micro-spin if adaptive wait policy is configured */
  if (ring && (ring->wait_policy & RBIPC_WAIT_ADAPTIVE)) {
    if (rbipc_io_spin_wait(rbipc_io_pred_slot_ready, &ctx)) {
      *out_next_t =
          atomic_load_explicit(&hdr->read_ticket, memory_order_relaxed);
      return RBIPC_OK;
    }
  }

  /* Tier 1: Hardware-assisted monitor-wait (WAITPKG / WFET) */
  if (rbipc_io_hardware_wait(ring, &slot->sequence, rbipc_io_pred_slot_ready, &ctx)) {
    *out_next_t =
        atomic_load_explicit(&hdr->read_ticket, memory_order_relaxed);
    return RBIPC_OK;
  }

  /* Phase 2: Passive Futex Sleep with Dekker-style double-check */
  rbipc_sync_register_waiter(&hdr->futex_waiters);

  uint64_t seq = atomic_load_explicit(&slot->sequence, memory_order_acquire);
  if (RBIPC_LIKELY(rbipc_slot_is_ready(seq, t))) {
    rbipc_sync_deregister_waiter(&hdr->futex_waiters);
    *out_next_t = atomic_load_explicit(&hdr->read_ticket, memory_order_relaxed);
    return RBIPC_OK;
  }
  if (RBIPC_UNLIKELY(rbipc_ring_is_drained_on_shutdown(hdr, t))) {
    rbipc_sync_deregister_waiter(&hdr->futex_waiters);
    return RBIPC_ERR_SHUTDOWN;
  }

  int rc = rbipc_sync_wait_slot_registered(
      sync_state, &slot->sequence, seq, &hdr->futex_seq,
      &hdr->futex_waiters, &hdr->shutdown_flag);
  if (RBIPC_UNLIKELY(rc != 0)) {
    return rc;
  }

  /* Unified ticket-hole audit & healing */
  rbipc_audit_check_and_heal_ticket_hole(hdr, slot, t, sync_state);

  *out_next_t = atomic_load_explicit(&hdr->read_ticket, memory_order_relaxed);
  return RBIPC_OK;
}

/* ============================================================================
 * Public Single-Item Write Interface
 * ============================================================================
 */

RBIPC_LEAF
int rbipc_reserve_write_timeout(rbipc_ring_t *RBIPC_RESTRICT ring, uint32_t len,
                                uint64_t timeout_ns,
                                void **RBIPC_RESTRICT out_buf,
                                uint64_t *RBIPC_RESTRICT ticket) {
  if (RBIPC_UNLIKELY(!ring || !ring->hdr || !out_buf || !ticket ||
                     len > ring->hdr->slot_size)) {
    return RBIPC_ERR_INVAL;
  }

  rbipc_shm_header_t *hdr = ring->hdr;
  const uint32_t mask = ring->cached_capacity_mask;
  const uint32_t cap = hdr->capacity;
  rbipc_slot_t *slots =
      (rbipc_slot_t *)RBIPC_ASSUME_ALIGNED(ring->slots, RBIPC_CACHE_LINE);

  rbipc_sync_state_t sync_state;
  rbipc_sync_state_init(&sync_state, timeout_ns);

  /* Bounded FAA Fast Path for Unbounded Blocking Writes (Nikolaev SCQ Pattern) */
  if (timeout_ns == UINT64_MAX) {
    for (;;) {
      if (RBIPC_UNLIKELY(rbipc_ring_is_shutdown(hdr))) {
        return RBIPC_ERR_SHUTDOWN;
      }

      uint64_t wt = atomic_load_explicit(&hdr->write_ticket, memory_order_relaxed);
      uint64_t rt = atomic_load_explicit(&hdr->read_ticket, memory_order_relaxed);

      if (RBIPC_LIKELY(wt < rt + (uint64_t)cap)) {
        uint64_t t = atomic_fetch_add_explicit(&hdr->write_ticket, 1,
                                               memory_order_acq_rel);
        rbipc_slot_t *slot = &slots[t & mask];
        uint64_t seq = atomic_load_explicit(&slot->sequence, memory_order_acquire);

        if (RBIPC_LIKELY(rbipc_slot_is_vacant(seq, t))) {
          rbipc_io_complete_write_reservation(ring, slot, t, out_buf, ticket);
          return RBIPC_OK;
        }

        /* Slot is behind: wait specifically on slot t until consumer frees it */
        uint64_t dummy_t = t;
        while (!rbipc_slot_is_vacant(seq, t)) {
          if (RBIPC_UNLIKELY(rbipc_ring_is_shutdown(hdr))) {
            return RBIPC_ERR_SHUTDOWN;
          }
          int rc = rbipc_io_handle_write_backoff(ring, &sync_state, hdr, slot, t,
                                                 timeout_ns, &dummy_t);
          if (RBIPC_UNLIKELY(rc != RBIPC_OK)) {
            return rc;
          }
          seq = atomic_load_explicit(&slot->sequence, memory_order_acquire);
        }
        rbipc_io_complete_write_reservation(ring, slot, t, out_buf, ticket);
        return RBIPC_OK;
      }

      /* Ring is full: wait on oldest unvacant slot */
      rbipc_slot_t *full_slot = &slots[wt & mask];
      uint64_t dummy_wt = wt;
      int rc = rbipc_io_handle_write_backoff(ring, &sync_state, hdr, full_slot, wt,
                                             timeout_ns, &dummy_wt);
      if (RBIPC_UNLIKELY(rc != RBIPC_OK)) {
        return rc;
      }
    }
  }

  /* Standard Bounded CAS Path for Finite-Timeout and Nonblocking Writes */
  uint64_t t = atomic_load_explicit(&hdr->write_ticket, memory_order_relaxed);
  for (;;) {
    if (RBIPC_UNLIKELY(rbipc_ring_is_shutdown(hdr))) {
      return RBIPC_ERR_SHUTDOWN;
    }

    rbipc_slot_t *slot = &slots[t & mask];
    uint64_t seq = atomic_load_explicit(&slot->sequence, memory_order_acquire);

    if (RBIPC_LIKELY(rbipc_slot_is_vacant(seq, t))) {
      if (RBIPC_LIKELY(rbipc_io_try_claim_write_ticket(hdr, &t, 1))) {
        rbipc_io_complete_write_reservation(ring, slot, t, out_buf, ticket);
        return RBIPC_OK;
      }
      rbipc_cpu_pause();
    } else if (rbipc_seq_is_behind(seq, t)) {
      int rc = rbipc_io_handle_write_backoff(ring, &sync_state, hdr, slot, t,
                                             timeout_ns, &t);
      if (RBIPC_UNLIKELY(rc != RBIPC_OK)) {
        return rc;
      }
    } else {
      if (sync_state.has_deadline &&
          RBIPC_UNLIKELY(rbipc_clock_monotonic_ns() >= sync_state.deadline_ns)) {
        return (timeout_ns == 0) ? RBIPC_ERR_FULL : RBIPC_ERR_TIMEOUT;
      }
      t = atomic_load_explicit(&hdr->write_ticket, memory_order_relaxed);
      rbipc_cpu_pause();
    }
  }
}

RBIPC_LEAF
int rbipc_reserve_write(rbipc_ring_t *RBIPC_RESTRICT ring, uint32_t len,
                        void **RBIPC_RESTRICT out_buf,
                        uint64_t *RBIPC_RESTRICT ticket) {
  return rbipc_reserve_write_timeout(ring, len, UINT64_MAX, out_buf, ticket);
}

RBIPC_LEAF
int rbipc_reserve_write_nonblock(rbipc_ring_t *RBIPC_RESTRICT ring,
                                 uint32_t len, void **RBIPC_RESTRICT out_buf,
                                 uint64_t *RBIPC_RESTRICT ticket) {
  return rbipc_reserve_write_timeout(ring, len, 0, out_buf, ticket);
}

RBIPC_LEAF
int rbipc_commit_write(rbipc_ring_t *RBIPC_RESTRICT ring, uint64_t ticket,
                       uint32_t written_len) {
  if (RBIPC_UNLIKELY(!ring || !ring->hdr ||
                     written_len > ring->cached_slot_size)) {
    return RBIPC_ERR_INVAL;
  }

  rbipc_shm_header_t *hdr = ring->hdr;
  uint32_t idx = (uint32_t)(ticket & (uint64_t)ring->cached_capacity_mask);
  rbipc_slot_t *slot =
      (rbipc_slot_t *)RBIPC_ASSUME_ALIGNED(&ring->slots[idx], RBIPC_CACHE_LINE);

  rbipc_slot_commit(slot, ticket, written_len);

  atomic_signal_fence(memory_order_seq_cst);
  if (RBIPC_UNLIKELY(rbipc_sync_has_waiters(&hdr->futex_waiters))) {
    rbipc_futex_wake_slot(&slot->sequence, 1);
    rbipc_sync_wake_one(&hdr->futex_seq, &hdr->futex_waiters);
  }
  rbipc_reactor_signal(ring);

  return RBIPC_OK;
}

RBIPC_LEAF
int rbipc_abort_write(rbipc_ring_t *RBIPC_RESTRICT ring, uint64_t ticket) {
  if (RBIPC_UNLIKELY(!ring || !ring->hdr)) {
    return RBIPC_ERR_INVAL;
  }

  rbipc_shm_header_t *hdr = ring->hdr;
  uint32_t idx = (uint32_t)(ticket & (uint64_t)ring->cached_capacity_mask);
  rbipc_slot_t *slot =
      (rbipc_slot_t *)RBIPC_ASSUME_ALIGNED(&ring->slots[idx], RBIPC_CACHE_LINE);

  if (rbipc_slot_poison(slot, ticket)) {
    atomic_fetch_add_explicit(&hdr->poisoned_slots_count, 1,
                              memory_order_relaxed);
    atomic_signal_fence(memory_order_seq_cst);
    if (RBIPC_UNLIKELY(rbipc_sync_has_waiters(&hdr->futex_waiters))) {
      rbipc_sync_wake_all(&hdr->futex_seq, &hdr->futex_waiters);
    }
    rbipc_reactor_signal(ring);
    return RBIPC_OK;
  }

  return RBIPC_ERR_BUSY;
}

/* ============================================================================
 * Public Single-Item Read Interface
 * ============================================================================
 */

RBIPC_LEAF
int rbipc_read_acquire_timeout(rbipc_ring_t *RBIPC_RESTRICT ring,
                               uint64_t timeout_ns,
                               const void **RBIPC_RESTRICT out_buf,
                               uint32_t *RBIPC_RESTRICT out_len,
                               uint64_t *RBIPC_RESTRICT ticket) {
  if (RBIPC_UNLIKELY(!ring || !ring->hdr || !out_buf || !out_len || !ticket)) {
    return RBIPC_ERR_INVAL;
  }

  rbipc_shm_header_t *hdr = ring->hdr;
  const uint32_t mask = ring->cached_capacity_mask;
  rbipc_slot_t *slots =
      (rbipc_slot_t *)RBIPC_ASSUME_ALIGNED(ring->slots, RBIPC_CACHE_LINE);
  rbipc_sync_state_t sync_state;
  rbipc_sync_state_init(&sync_state, timeout_ns);

  /* Bounded FAA Fast Path for Unbounded Blocking Reads (Nikolaev SCQ Pattern) */
  if (timeout_ns == UINT64_MAX) {
    for (;;) {
      uint64_t rt = atomic_load_explicit(&hdr->read_ticket, memory_order_relaxed);
      if (RBIPC_UNLIKELY(rbipc_ring_is_drained_on_shutdown(hdr, rt))) {
        return RBIPC_ERR_SHUTDOWN;
      }

      uint64_t wt = atomic_load_explicit(&hdr->write_ticket, memory_order_relaxed);

      if (RBIPC_LIKELY(rt < wt)) {
        uint64_t t = atomic_fetch_add_explicit(&hdr->read_ticket, 1,
                                               memory_order_acq_rel);
        rbipc_slot_t *slot = &slots[t & mask];
        uint64_t seq = atomic_load_explicit(&slot->sequence, memory_order_acquire);

        if (RBIPC_LIKELY(rbipc_slot_is_ready(seq, t))) {
          return rbipc_io_complete_read_acquisition(ring, slot, t, out_buf,
                                                    out_len, ticket);
        }

        /* Slot is behind: wait specifically on slot t until producer commits it */
        uint64_t dummy_t = t;
        while (!rbipc_slot_is_ready(seq, t)) {
          if (RBIPC_UNLIKELY(rbipc_ring_is_drained_on_shutdown(hdr, t))) {
            return RBIPC_ERR_SHUTDOWN;
          }
          int rc = rbipc_io_handle_read_backoff(ring, &sync_state, hdr, slot, t,
                                                timeout_ns, &dummy_t);
          if (RBIPC_UNLIKELY(rc != RBIPC_OK)) {
            return rc;
          }
          seq = atomic_load_explicit(&slot->sequence, memory_order_acquire);
        }
        return rbipc_io_complete_read_acquisition(ring, slot, t, out_buf,
                                                  out_len, ticket);
      }

      /* Ring is empty: wait on current read slot */
      rbipc_slot_t *empty_slot = &slots[rt & mask];
      uint64_t dummy_rt = rt;
      int rc = rbipc_io_handle_read_backoff(ring, &sync_state, hdr, empty_slot, rt,
                                            timeout_ns, &dummy_rt);
      if (RBIPC_UNLIKELY(rc != RBIPC_OK)) {
        return rc;
      }
    }
  }

  /* Standard Bounded CAS Path for Finite-Timeout and Nonblocking Reads */
  uint64_t t = atomic_load_explicit(&hdr->read_ticket, memory_order_relaxed);
  for (;;) {
    if (RBIPC_UNLIKELY(rbipc_ring_is_drained_on_shutdown(hdr, t))) {
      return RBIPC_ERR_SHUTDOWN;
    }

    rbipc_slot_t *slot = &slots[t & mask];
    uint64_t seq = atomic_load_explicit(&slot->sequence, memory_order_acquire);

    if (RBIPC_LIKELY(rbipc_slot_is_ready(seq, t))) {
      if (RBIPC_LIKELY(rbipc_io_try_claim_read_ticket(hdr, &t, 1))) {
        return rbipc_io_complete_read_acquisition(ring, slot, t, out_buf,
                                                  out_len, ticket);
      }
      rbipc_cpu_pause();
    } else if (rbipc_seq_is_behind(seq, t + 1)) {
      int rc = rbipc_io_handle_read_backoff(ring, &sync_state, hdr, slot, t,
                                            timeout_ns, &t);
      if (RBIPC_UNLIKELY(rc != RBIPC_OK)) {
        return rc;
      }
    } else {
      if (sync_state.has_deadline &&
          RBIPC_UNLIKELY(rbipc_clock_monotonic_ns() >= sync_state.deadline_ns)) {
        return (timeout_ns == 0) ? RBIPC_ERR_EMPTY : RBIPC_ERR_TIMEOUT;
      }
      t = atomic_load_explicit(&hdr->read_ticket, memory_order_relaxed);
      rbipc_cpu_pause();
    }
  }
}

RBIPC_LEAF
int rbipc_read_acquire(rbipc_ring_t *RBIPC_RESTRICT ring,
                       const void **RBIPC_RESTRICT out_buf,
                       uint32_t *RBIPC_RESTRICT out_len,
                       uint64_t *RBIPC_RESTRICT ticket) {
  return rbipc_read_acquire_timeout(ring, UINT64_MAX, out_buf, out_len, ticket);
}

RBIPC_LEAF
int rbipc_read_acquire_nonblock(rbipc_ring_t *RBIPC_RESTRICT ring,
                                const void **RBIPC_RESTRICT out_buf,
                                uint32_t *RBIPC_RESTRICT out_len,
                                uint64_t *RBIPC_RESTRICT ticket) {
  return rbipc_read_acquire_timeout(ring, 0, out_buf, out_len, ticket);
}

RBIPC_LEAF
int rbipc_read_release(rbipc_ring_t *RBIPC_RESTRICT ring, uint64_t ticket) {
  if (RBIPC_UNLIKELY(!ring || !ring->hdr)) {
    return RBIPC_ERR_INVAL;
  }

  rbipc_shm_header_t *hdr = ring->hdr;
  const uint32_t mask = ring->cached_capacity_mask;
  const uint32_t cap = hdr->capacity;
  rbipc_slot_t *slot = (rbipc_slot_t *)RBIPC_ASSUME_ALIGNED(
      &ring->slots[ticket & mask], RBIPC_CACHE_LINE);

  bool released = rbipc_slot_release_ex(slot, ticket, cap, ring->cached_pid);

  atomic_signal_fence(memory_order_seq_cst);
  if (RBIPC_UNLIKELY(rbipc_sync_has_waiters(&hdr->write_waiters))) {
    rbipc_futex_wake_slot(&slot->sequence, 1);
    rbipc_sync_wake_one(&hdr->write_futex_seq, &hdr->write_waiters);
  }
  rbipc_reactor_signal(ring);

  return released ? RBIPC_OK : RBIPC_ERR_TIMEOUT;
}

/* ============================================================================
 * Convenience Single-Slot Copy I/O Routines
 * ============================================================================
 */

RBIPC_LEAF
int rbipc_write_timeout(rbipc_ring_t *RBIPC_RESTRICT ring,
                        const void *RBIPC_RESTRICT buf, uint32_t len,
                        uint64_t timeout_ns) {
  if (RBIPC_UNLIKELY(!ring || !buf)) {
    return RBIPC_ERR_INVAL;
  }

  void *slot_buf = NULL;
  uint64_t ticket = 0;
  int rc = rbipc_reserve_write_timeout(ring, len, timeout_ns, &slot_buf, &ticket);
  if (RBIPC_UNLIKELY(rc != RBIPC_OK)) {
    return rc;
  }

  if (len > 0) {
    memcpy(slot_buf, buf, len);
  }
  return rbipc_commit_write(ring, ticket, len);
}

RBIPC_LEAF
int rbipc_write(rbipc_ring_t *RBIPC_RESTRICT ring,
                const void *RBIPC_RESTRICT buf, uint32_t len) {
  return rbipc_write_timeout(ring, buf, len, UINT64_MAX);
}

RBIPC_LEAF
int rbipc_write_nonblock(rbipc_ring_t *RBIPC_RESTRICT ring,
                         const void *RBIPC_RESTRICT buf, uint32_t len) {
  return rbipc_write_timeout(ring, buf, len, 0);
}

RBIPC_LEAF
int rbipc_read_timeout(rbipc_ring_t *RBIPC_RESTRICT ring,
                       void *RBIPC_RESTRICT buf, uint32_t max_len,
                       uint32_t *RBIPC_RESTRICT out_len,
                       uint64_t timeout_ns) {
  if (RBIPC_UNLIKELY(!ring || !buf)) {
    return RBIPC_ERR_INVAL;
  }

  const void *slot_buf = NULL;
  uint32_t actual_len = 0;
  uint64_t ticket = 0;
  int rc = rbipc_read_acquire_timeout(ring, timeout_ns, &slot_buf, &actual_len, &ticket);
  if (RBIPC_UNLIKELY(rc != RBIPC_OK)) {
    if (rc == RBIPC_ERR_POISONED) {
      (void)rbipc_read_release(ring, ticket);
    }
    return rc;
  }

  uint32_t copy_len = (actual_len < max_len) ? actual_len : max_len;
  if (copy_len > 0) {
    memcpy(buf, slot_buf, copy_len);
  }
  if (out_len) {
    *out_len = actual_len;
  }

  int rel_rc = rbipc_read_release(ring, ticket);
  if (RBIPC_UNLIKELY(rel_rc != RBIPC_OK)) {
    return rel_rc;
  }
  return RBIPC_OK;
}

RBIPC_LEAF
int rbipc_read(rbipc_ring_t *RBIPC_RESTRICT ring,
               void *RBIPC_RESTRICT buf, uint32_t max_len,
               uint32_t *RBIPC_RESTRICT out_len) {
  return rbipc_read_timeout(ring, buf, max_len, out_len, UINT64_MAX);
}

RBIPC_LEAF
int rbipc_read_nonblock(rbipc_ring_t *RBIPC_RESTRICT ring,
                        void *RBIPC_RESTRICT buf, uint32_t max_len,
                        uint32_t *RBIPC_RESTRICT out_len) {
  return rbipc_read_timeout(ring, buf, max_len, out_len, 0);
}
