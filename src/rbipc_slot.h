/**
 * @file rbipc_slot.h
 * @brief Slot state machine, sequence progression, and peer crash recovery routines.
 *
 * @details Implements atomic transitions between EMPTY, RESERVED, COMMITTED, and POISONED
 * states using acquire/release memory semantics, and provides process liveness auditing.
 *
 * @author Christian Santarelli
 * @date 2026
 * @copyright Apache License 2.0
 */

#ifndef RBIPC_SLOT_H
#define RBIPC_SLOT_H

#include "rbipc.h"
#include "rbipc_attr.h"

#include <sys/types.h>
#include <stdbool.h>

/**
 * @brief Initialize all slot descriptors across the shared memory slot table.
 *
 * @details Resets sequence numbers to match their slot index, marks state as EMPTY,
 * and clears producer PIDs and length counters.
 *
 * @param[out] slots    Pointer to the base of the cache-line aligned slot descriptor array.
 * @param[in]  capacity Total number of slots to initialize.
 */
RBIPC_LEAF
void rbipc_slot_init_table(rbipc_slot_t * RBIPC_RESTRICT slots, uint32_t capacity);

/**
 * @brief Audit whether a peer process with the specified PID is currently alive.
 *
 * @details Probes process existence via `kill(pid, 0)`.
 *
 * @param[in] pid Process identifier to probe.
 *
 * @return true if the process exists or permission is denied (indicating process is alive);
 *         false if ESRCH is returned (confirming process is dead/terminated).
 */
RBIPC_NODISCARD RBIPC_LEAF
bool rbipc_slot_is_peer_alive(pid_t pid);

/**
 * @brief Atomically transition a slot descriptor to RESERVED state.
 *
 * @details Sets the producer PID and stores @ref RBIPC_SLOT_RESERVED with relaxed ordering.
 *
 * @param[in,out] slot Direct pointer to slot descriptor.
 * @param[in]     pid  Process identifier of reserving producer.
 */
RBIPC_LEAF
void rbipc_slot_mark_reserved(rbipc_slot_t *slot, pid_t pid);

/**
 * @brief Atomically commit a written slot payload and publish it to waiting consumers.
 *
 * @details Stores payload byte length with relaxed memory ordering, updates slot state to
 * @ref RBIPC_SLOT_COMMITTED, and publishes `sequence = ticket + 1` with release memory ordering.
 *
 * @param[in,out] slot   Direct pointer to slot descriptor.
 * @param[in]     ticket Reserving ticket number.
 * @param[in]     len    Actual payload byte length written.
 */
RBIPC_LEAF
void rbipc_slot_commit(rbipc_slot_t *slot, uint32_t ticket, uint32_t len);

/**
 * @brief Transition an uncommitted or orphaned slot to POISONED state.
 *
 * @details Executes a compare-and-swap from RESERVED to POISONED. If successful, advances
 * the sequence to `ticket + 1` so consumers can safely bypass the slot without deadlock.
 *
 * @param[in,out] slot   Direct pointer to slot descriptor.
 * @param[in]     ticket Reserving ticket number.
 *
 * @return true if slot was successfully transitioned to POISONED; false if already committed/released.
 */
RBIPC_LEAF
bool rbipc_slot_poison(rbipc_slot_t *slot, uint32_t ticket);

/**
 * @brief Release a consumed slot back to the ring buffer for producer recycling.
 *
 * @details Resets state to @ref RBIPC_SLOT_EMPTY, clears PID and length, and advances
 * the sequence to `ticket + capacity` with release memory ordering.
 *
 * @param[in,out] slot     Direct pointer to slot descriptor.
 * @param[in]     ticket   Acquired ticket number being released.
 * @param[in]     capacity Total ring buffer capacity (turn step distance).
 */
RBIPC_LEAF
void rbipc_slot_release(rbipc_slot_t *slot, uint32_t ticket, uint32_t capacity);

#endif /* RBIPC_SLOT_H */
