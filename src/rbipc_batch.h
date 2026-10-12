/**
 * @file rbipc_batch.h
 * @brief B-Queue multi-message vector batching operations (Scatter-Gather I/O).
 *
 * @details Implements contiguous multi-slot reservations, commits,
 * acquisitions, and releases using @ref rbipc_iovec_t and @ref rbipc_rovec_t
 * vector descriptors, amortizing atomic CAS operations, cache-line invalidation
 * traffic, and kernel synchronization transitions.
 *
 * Grounded in systems literature:
 * - J. Wang, K. Zhang, X. Tang, B. Hua (IJPP 2013), "B-Queue: Efficient and
 * Practical Queuing for Fast Core-to-Core Communication".
 *
 * @author Seintian
 * @date 2026
 * @copyright Apache License 2.0
 */

#ifndef RBIPC_BATCH_H
#define RBIPC_BATCH_H

#include "rbipc.h"
#include "rbipc_attr.h"
#include <stdint.h>

/**
 * @brief Reserve a batch of contiguous slots for writing with an optional
 * timeout deadline.
 */
RBIPC_NODISCARD RBIPC_LEAF int
rbipc_reserve_write_batch_timeout(rbipc_ring_t *RBIPC_RESTRICT ring,
                                  uint32_t count, uint64_t timeout_ns,
                                  rbipc_iovec_t *RBIPC_RESTRICT iovecs,
                                  uint32_t *RBIPC_RESTRICT out_reserved);

/**
 * @brief Reserve a batch of contiguous slots for writing (Blocking).
 */
RBIPC_NODISCARD RBIPC_LEAF int
rbipc_reserve_write_batch(rbipc_ring_t *RBIPC_RESTRICT ring, uint32_t count,
                          rbipc_iovec_t *RBIPC_RESTRICT iovecs,
                          uint32_t *RBIPC_RESTRICT out_reserved);

/**
 * @brief Reserve a batch of contiguous slots for writing (Non-blocking).
 */
RBIPC_NODISCARD RBIPC_LEAF int
rbipc_reserve_write_batch_nonblock(rbipc_ring_t *RBIPC_RESTRICT ring,
                                   uint32_t count,
                                   rbipc_iovec_t *RBIPC_RESTRICT iovecs,
                                   uint32_t *RBIPC_RESTRICT out_reserved);

/**
 * @brief Atomically commit and publish a batch of previously reserved write
 * slots.
 */
RBIPC_NODISCARD RBIPC_LEAF int
rbipc_commit_write_batch(rbipc_ring_t *RBIPC_RESTRICT ring, uint32_t count,
                         const uint64_t *RBIPC_RESTRICT tickets,
                         const uint32_t *RBIPC_RESTRICT lens);

/**
 * @brief Acquire a batch of committed slots for reading with an optional
 * timeout deadline.
 */
RBIPC_NODISCARD RBIPC_LEAF int
rbipc_read_acquire_batch_timeout(rbipc_ring_t *RBIPC_RESTRICT ring,
                                 uint32_t count, uint64_t timeout_ns,
                                 rbipc_rovec_t *RBIPC_RESTRICT rovecs,
                                 uint32_t *RBIPC_RESTRICT out_acquired);

/**
 * @brief Acquire a batch of committed slots for reading (Blocking).
 */
RBIPC_NODISCARD RBIPC_LEAF int
rbipc_read_acquire_batch(rbipc_ring_t *RBIPC_RESTRICT ring, uint32_t count,
                         rbipc_rovec_t *RBIPC_RESTRICT rovecs,
                         uint32_t *RBIPC_RESTRICT out_acquired);

/**
 * @brief Acquire a batch of committed slots for reading (Non-blocking).
 */
RBIPC_NODISCARD RBIPC_LEAF int
rbipc_read_acquire_batch_nonblock(rbipc_ring_t *RBIPC_RESTRICT ring,
                                  uint32_t count,
                                  rbipc_rovec_t *RBIPC_RESTRICT rovecs,
                                  uint32_t *RBIPC_RESTRICT out_acquired);

/**
 * @brief Atomically release a batch of previously consumed read slots back to
 * the ring.
 */
RBIPC_NODISCARD RBIPC_LEAF int
rbipc_read_release_batch(rbipc_ring_t *RBIPC_RESTRICT ring, uint32_t count,
                         const uint64_t *RBIPC_RESTRICT tickets);

#endif /* RBIPC_BATCH_H */