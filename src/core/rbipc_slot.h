/**
 * @file rbipc_slot.h
 * @brief Slot state machine, sequence progression, and peer crash recovery
 * routines.
 *
 * @details Implements atomic transitions between EMPTY, RESERVED, COMMITTED,
 * and POISONED states using acquire/release memory semantics, and provides
 * process liveness auditing.
 *
 * @author Seintian
 * @date 2026
 * @copyright Apache License 2.0
 */

#ifndef RBIPC_SLOT_H
#define RBIPC_SLOT_H

#include "rbipc.h"
#include "rbipc_attr.h"
#include "rbipc_audit.h"

#include <stdbool.h>
#include <sys/types.h>

/**
 * @brief Initialize all slot descriptors across the shared memory slot table.
 *
 * @details Resets sequence numbers to match their slot index, marks state as
 * EMPTY, and clears producer PIDs and length counters.
 *
 * @param[out] slots    Pointer to the base of the cache-line aligned slot
 * descriptor array.
 * @param[in]  capacity Total number of slots to initialize.
 */
RBIPC_INTERNAL RBIPC_LEAF
void rbipc_slot_init_table(rbipc_slot_t *RBIPC_RESTRICT slots,
                           uint32_t capacity);

/**
 * @brief Audit whether a peer process with the specified PID is currently
 * alive.
 *
 * @details Delegates to rbipc_audit_is_peer_alive.
 *
 * @param[in] pid Process identifier to probe.
 *
 * @return true if the process exists or permission is denied (indicating
 * process is alive); false if ESRCH is returned (confirming process is
 * dead/terminated).
 */
RBIPC_INLINE RBIPC_NODISCARD bool rbipc_slot_is_peer_alive(pid_t pid) {
  return rbipc_audit_is_peer_alive(pid);
}

/**
 * @brief Atomically transition a slot descriptor to RESERVING state.
 *
 * @details Pre-reserves slot before acquiring the write ticket to close PID
 * registration races.
 *
 * @param[in,out] slot Direct pointer to slot descriptor.
 * @param[in]     pid  Process identifier of reserving producer.
 */
RBIPC_INTERNAL RBIPC_LEAF
void rbipc_slot_mark_reserving(rbipc_slot_t *slot, pid_t pid);

/**
 * @brief Atomically transition a slot descriptor to RESERVED state.
 *
 * @details Sets the producer PID and stores @ref RBIPC_SLOT_RESERVED with
 * relaxed ordering.
 *
 * @param[in,out] slot Direct pointer to slot descriptor.
 * @param[in]     pid  Process identifier of reserving producer.
 */
RBIPC_INTERNAL RBIPC_LEAF
void rbipc_slot_mark_reserved(rbipc_slot_t *slot, pid_t pid);

/**
 * @brief Atomically commit a written slot payload and publish it to waiting
 * consumers.
 *
 * @details Stores payload byte length with relaxed memory ordering, updates
 * slot state to
 * @ref RBIPC_SLOT_COMMITTED, and publishes `sequence = ticket + 1` with release
 * memory ordering.
 *
 * @param[in,out] slot   Direct pointer to slot descriptor.
 * @param[in]     ticket Reserving ticket number.
 * @param[in]     len    Actual payload byte length written.
 */
RBIPC_INTERNAL RBIPC_LEAF
void rbipc_slot_commit(rbipc_slot_t *slot, uint64_t ticket, uint32_t len);

/**
 * @brief Transition an uncommitted or orphaned slot to POISONED state.
 *
 * @details Executes a compare-and-swap from RESERVED to POISONED. If
 * successful, advances the sequence to `ticket + 1` so consumers can safely
 * bypass the slot without deadlock.
 *
 * @param[in,out] slot   Direct pointer to slot descriptor.
 * @param[in]     ticket Reserving ticket number.
 *
 * @return true if slot was successfully transitioned to POISONED; false if
 * already committed/released.
 */
RBIPC_INTERNAL RBIPC_LEAF
bool rbipc_slot_poison(rbipc_slot_t *slot, uint64_t ticket);

/**
 * @brief Stamp slot with consumer ownership on acquisition.
 *
 * @param[in,out] slot         Direct pointer to slot descriptor.
 * @param[in]     pid          Process identifier of acquiring consumer.
 * @param[in]     timestamp_ns Monotonic timestamp in nanoseconds.
 */
RBIPC_INTERNAL RBIPC_LEAF
void rbipc_slot_mark_acquired(rbipc_slot_t *slot, pid_t pid,
                              uint64_t timestamp_ns);

/**
 * @brief Recovers an unreleased slot held by a dead consumer peer.
 *
 * @details If holding consumer PID is dead, resets slot state to EMPTY,
 * clears consumer/producer fields, and advances slot sequence to make it
 * vacant.
 *
 * @param[in,out] slot   Direct pointer to slot descriptor.
 * @param[in]     ticket Producer ticket attempting to claim this slot.
 *
 * @return true if slot was recovered from dead consumer; false otherwise.
 */
RBIPC_INTERNAL RBIPC_LEAF
bool rbipc_slot_recover_dead_consumer(rbipc_slot_t *slot, uint64_t ticket);

/**
 * @brief Recovers an unreleased slot held past its lease timeout.
 *
 * @param[in,out] slot                  Direct pointer to slot descriptor.
 * @param[in]     target_vacant_ticket Target sequence counter making slot
 * vacant for next producer.
 * @param[in]     lease_timeout_ns      Configured lease duration in
 * nanoseconds.
 * @param[in]     now_ns                Current monotonic timestamp in
 * nanoseconds.
 *
 * @return true if slot lease was revoked; false otherwise.
 */
RBIPC_INTERNAL RBIPC_LEAF
bool rbipc_slot_recover_expired_lease(rbipc_slot_t *slot,
                                      uint64_t target_vacant_ticket,
                                      uint64_t lease_timeout_ns,
                                      uint64_t now_ns);

/**
 * @brief Release a consumed slot back to the ring buffer with expected consumer
 * PID validation.
 *
 * @details Resets state to @ref RBIPC_SLOT_EMPTY, clears PID and length, and
 * advances the sequence to `ticket + capacity` with release memory ordering.
 *
 * @param[in,out] slot         Direct pointer to slot descriptor.
 * @param[in]     ticket       Acquired ticket number being released.
 * @param[in]     capacity     Total ring buffer capacity (turn step distance).
 * @param[in]     expected_pid Expected consumer PID (or 0 to bypass PID
 * validation).
 *
 * @return true on successful release; false if consumer PID mismatch.
 */
RBIPC_INTERNAL RBIPC_LEAF
bool rbipc_slot_release_ex(rbipc_slot_t *slot, uint64_t ticket,
                           uint32_t capacity, pid_t expected_pid);

/**
 * @brief Release a consumed slot back to the ring buffer for producer
 * recycling.
 *
 * @details Resets state to @ref RBIPC_SLOT_EMPTY, clears PID and length, and
 * advances the sequence to `ticket + capacity` with release memory ordering.
 *
 * @param[in,out] slot     Direct pointer to slot descriptor.
 * @param[in]     ticket   Acquired ticket number being released.
 * @param[in]     capacity Total ring buffer capacity (turn step distance).
 */
RBIPC_INLINE void rbipc_slot_release(rbipc_slot_t *slot, uint64_t ticket,
                                     uint32_t capacity) {
  (void)rbipc_slot_release_ex(slot, ticket, capacity, 0);
}

#endif /* RBIPC_SLOT_H */
