/**
 * @file rbipc_batch.h
 * @brief B-Queue multi-message vector batching operations (Scatter-Gather I/O).
 *
 * @details Implements contiguous multi-slot reservations, commits, acquisitions, and releases
 * using @ref rbipc_iovec_t and @ref rbipc_rovec_t vector descriptors, amortizing atomic CAS
 * operations, cache-line invalidation traffic, and kernel synchronization transitions.
 *
 * Grounded in systems literature:
 * - J. Yang & H. Zhou (ISCA 2012), "B-Queue: Efficient and Practical Queuing for Fast Core-to-Core Communication".
 *
 * @author Christian Santarelli
 * @date 2026
 * @copyright Apache License 2.0
 */

#ifndef RBIPC_BATCH_H
#define RBIPC_BATCH_H

#include "rbipc.h"
#include "rbipc_attr.h"
#include <stdint.h>

/**
 * @brief Reserve a batch of contiguous slots for writing with an optional timeout deadline.
 *
 * @param[in,out] ring         Ring buffer handle.
 * @param[in]     count        Requested slot count (clamped to capacity).
 * @param[in]     timeout_ns   Timeout deadline in nanoseconds (0 = nonblock, UINT64_MAX = unbounded).
 * @param[out]    iovecs       Caller-allocated array of at least @p count vector descriptors.
 * @param[out]    out_reserved Location to store number of slots successfully reserved (0 < n <= count).
 *
 * @return RBIPC_OK on success, or appropriate error code.
 */
RBIPC_NODISCARD RBIPC_LEAF
int rbipc_reserve_write_batch_timeout(rbipc_ring_t * RBIPC_RESTRICT ring, uint32_t count,
                                      uint64_t timeout_ns,
                                      rbipc_iovec_t * RBIPC_RESTRICT iovecs,
                                      uint32_t * RBIPC_RESTRICT out_reserved);

/**
 * @brief Reserve a batch of contiguous slots for writing (Blocking).
 */
RBIPC_NODISCARD RBIPC_LEAF
int rbipc_reserve_write_batch(rbipc_ring_t * RBIPC_RESTRICT ring, uint32_t count,
                              rbipc_iovec_t * RBIPC_RESTRICT iovecs,
                              uint32_t * RBIPC_RESTRICT out_reserved);

/**
 * @brief Reserve a batch of contiguous slots for writing (Non-blocking).
 */
RBIPC_NODISCARD RBIPC_LEAF
int rbipc_reserve_write_batch_nonblock(rbipc_ring_t * RBIPC_RESTRICT ring, uint32_t count,
                                       rbipc_iovec_t * RBIPC_RESTRICT iovecs,
                                       uint32_t * RBIPC_RESTRICT out_reserved);

/**
 * @brief Atomically commit and publish a batch of previously reserved write slots.
 *
 * @param[in,out] ring         Ring buffer handle.
 * @param[in]     count        Number of slots to commit.
 * @param[in]     tickets      Array of ticket numbers to commit.
 * @param[in]     written_lens Array of actual written byte lengths for each slot.
 *
 * @return RBIPC_OK on success, or RBIPC_ERR_INVAL if lengths exceed slot capacity.
 */
RBIPC_NODISCARD RBIPC_LEAF
int rbipc_commit_write_batch(rbipc_ring_t * RBIPC_RESTRICT ring, uint32_t count,
                             const uint64_t * RBIPC_RESTRICT tickets,
                             const uint32_t * RBIPC_RESTRICT written_lens);

/**
 * @brief Acquire a batch of committed slots for reading with an optional timeout deadline.
 *
 * @param[in,out] ring         Ring buffer handle.
 * @param[in]     count        Requested slot count (clamped to capacity).
 * @param[in]     timeout_ns   Timeout deadline in nanoseconds (0 = nonblock, UINT64_MAX = unbounded).
 * @param[out]    rovecs       Caller-allocated array of at least @p count read descriptors.
 * @param[out]    out_acquired Location to store number of slots successfully acquired.
 *
 * @return RBIPC_OK on success, or appropriate error code.
 */
RBIPC_NODISCARD RBIPC_LEAF
int rbipc_read_acquire_batch_timeout(rbipc_ring_t * RBIPC_RESTRICT ring, uint32_t count,
                                     uint64_t timeout_ns,
                                     rbipc_rovec_t * RBIPC_RESTRICT rovecs,
                                     uint32_t * RBIPC_RESTRICT out_acquired);

/**
 * @brief Acquire a batch of committed slots for reading (Blocking).
 */
RBIPC_NODISCARD RBIPC_LEAF
int rbipc_read_acquire_batch(rbipc_ring_t * RBIPC_RESTRICT ring, uint32_t count,
                             rbipc_rovec_t * RBIPC_RESTRICT rovecs,
                             uint32_t * RBIPC_RESTRICT out_acquired);

/**
 * @brief Acquire a batch of committed slots for reading (Non-blocking).
 */
RBIPC_NODISCARD RBIPC_LEAF
int rbipc_read_acquire_batch_nonblock(rbipc_ring_t * RBIPC_RESTRICT ring, uint32_t count,
                                      rbipc_rovec_t * RBIPC_RESTRICT rovecs,
                                      uint32_t * RBIPC_RESTRICT out_acquired);

/**
 * @brief Atomically release a batch of previously consumed read slots back to the ring.
 *
 * @param[in,out] ring    Ring buffer handle.
 * @param[in]     count   Number of slots to release.
 * @param[in]     tickets Array of slot tickets to release.
 *
 * @return RBIPC_OK on success, or RBIPC_ERR_INVAL if parameters invalid.
 */
RBIPC_NODISCARD RBIPC_LEAF
int rbipc_read_release_batch(rbipc_ring_t * RBIPC_RESTRICT ring, uint32_t count,
                             const uint64_t * RBIPC_RESTRICT tickets);

#endif /* RBIPC_BATCH_H */