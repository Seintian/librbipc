/**
 * @file rbipc_waitfree.h
 * @brief Bounded Wait-Free Concurrent Queue (wCQ) helping mechanics.
 *
 * @details Implements Nikolaev & Ravindran's bounded wait-free helping algorithm (SPAA 2022).
 *          Provides dual-path execution: an ultra-fast lock-free ticket path and a bounded
 *          cooperative helping slow path with deterministic O(P) step complexity,
 *          eliminating thread starvation and tail latency spikes under OS preemption.
 *
 * @author Seintian
 * @date 2026
 * @copyright Apache License 2.0
 */

#ifndef RBIPC_WAITFREE_H
#define RBIPC_WAITFREE_H

#include "rbipc.h"
#include "rbipc_attr.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RBIPC_HELP_MAX_PEERS 64U
#define RBIPC_HELP_FAST_TRIES 32U

enum rbipc_help_state {
  RBIPC_HELP_IDLE = 0,
  RBIPC_HELP_POSTED = 1,
  RBIPC_HELP_IN_PROGRESS = 2,
  RBIPC_HELP_COMPLETED = 3,
  RBIPC_HELP_FAILED = 4,
};

enum rbipc_help_op {
  RBIPC_HELP_OP_WRITE = 1,
  RBIPC_HELP_OP_READ = 2,
};

/**
 * @struct rbipc_help_slot
 * @brief Cache-aligned cooperative helper slot descriptor in shared memory.
 */
typedef struct rbipc_help_slot {
  _Alignas(RBIPC_CACHE_LINE) _Atomic uint64_t ticket;           /**< Target ring slot ticket */
  _Atomic uint32_t state;            /**< enum rbipc_help_state */
  _Atomic uint32_t op;               /**< enum rbipc_help_op */
  _Atomic uint32_t payload_len;      /**< Length of payload */
  _Atomic uint32_t owner_pid;        /**< PID of requesting peer */
  _Atomic uint32_t helper_pid;       /**< PID of peer that rendered assistance */
  _Atomic uint32_t reserved0;
  _Atomic uint64_t payload_offset;   /**< Byte offset in data ring for payload */
#if RBIPC_CACHE_LINE > 40U
  uint8_t inline_staging[RBIPC_CACHE_LINE - 40U]; /**< Dynamic payload staging up to cache line size */
#else
  uint8_t inline_staging[8];
#endif
} rbipc_help_slot_t;

static_assert(sizeof(rbipc_help_slot_t) == RBIPC_CACHE_LINE,
              "rbipc_help_slot_t size must match RBIPC_CACHE_LINE");
static_assert(alignof(rbipc_help_slot_t) == RBIPC_CACHE_LINE,
              "rbipc_help_slot_t must be aligned to RBIPC_CACHE_LINE boundary");

/**
 * @brief Initialize all helper slots in the cooperative helping table.
 *
 * @param[out] table Array of @ref RBIPC_HELP_MAX_PEERS helper slots.
 * @param[in]  count Number of slots to initialize (up to @ref RBIPC_HELP_MAX_PEERS).
 */
RBIPC_INTERNAL void rbipc_waitfree_init_table(rbipc_help_slot_t *table, uint32_t count);

/**
 * @brief Registers the current process/thread in the cooperative helper array.
 *
 * @param[in,out] ring Ring buffer handle.
 * @return @ref RBIPC_OK on success, @ref RBIPC_ERR_FULL if helper array is saturated.
 */
RBIPC_INTERNAL RBIPC_NODISCARD int rbipc_waitfree_register_peer(rbipc_ring_t *ring);

/**
 * @brief Unregisters the current process/thread from the cooperative helper array.
 *
 * @param[in,out] ring Ring buffer handle.
 */
RBIPC_INTERNAL void rbipc_waitfree_unregister_peer(rbipc_ring_t *ring);

/**
 * @brief Actively scans cooperative helper slots to assist stalled concurrent peers.
 *
 * @param[in] ring Ring buffer handle.
 */
RBIPC_INTERNAL void rbipc_waitfree_cooperative_help(rbipc_ring_t *ring);

/**
 * @brief Publishes an operation to the cooperative slow path and awaits bounded completion.
 *
 * @param[in] ring       Ring buffer handle.
 * @param[in] ticket     Claimed ticket.
 * @param[in] op         Operation type (WRITE or READ).
 * @param[in] buf        Payload buffer.
 * @param[in] len        Payload length in bytes.
 * @param[in] timeout_ns Maximum timeout in nanoseconds (or UINT64_MAX).
 * @return @ref RBIPC_OK on success, error code on timeout/failure.
 */
RBIPC_INTERNAL RBIPC_NODISCARD int rbipc_waitfree_post_and_wait(
    rbipc_ring_t *ring, uint64_t ticket, uint32_t op, const void *buf,
    size_t len, uint64_t timeout_ns);

#ifdef __cplusplus
}
#endif

#endif /* RBIPC_WAITFREE_H */
