/**
 * @file rbipc_slot.h
 * @brief Slot state machine, sequence management, and dead-peer detection
 */

#ifndef RBIPC_SLOT_H
#define RBIPC_SLOT_H

#include "rbipc.h"
#include "rbipc_attr.h"

#include <sys/types.h>
#include <stdbool.h>

/**
 * @brief Initialize all slot descriptors in shared memory.
 *
 * @param slots Pointer to slot array in shared memory.
 * @param capacity Number of slots.
 */
RBIPC_LEAF
void rbipc_slot_init_table(rbipc_slot_t * RBIPC_RESTRICT slots, uint32_t capacity);

/**
 * @brief Check if process with given PID is still alive.
 *
 * @param pid Process ID to probe.
 * @return true if process exists or permissions prevent signal, false if ESRCH (dead).
 */
RBIPC_NODISCARD RBIPC_LEAF
bool rbipc_slot_is_peer_alive(pid_t pid);

/**
 * @brief Atomically mark a slot as reserved by the current process.
 *
 * @param slot Slot descriptor.
 * @param pid Current process PID.
 */
RBIPC_LEAF
void rbipc_slot_mark_reserved(rbipc_slot_t *slot, pid_t pid);

/**
 * @brief Atomically commit a reserved slot with published length and update sequence.
 *
 * @param slot Slot descriptor.
 * @param ticket Ticket identifier assigned to this slot.
 * @param len Byte length of committed payload.
 */
RBIPC_LEAF
void rbipc_slot_commit(rbipc_slot_t *slot, uint32_t ticket, uint32_t len);

/**
 * @brief Transition a slot to POISONED and advance sequence so consumers can skip it.
 *
 * @param slot Slot descriptor.
 * @param ticket Ticket identifier.
 * @return true if state transitioned from RESERVED to POISONED, false otherwise.
 */
RBIPC_LEAF
bool rbipc_slot_poison(rbipc_slot_t *slot, uint32_t ticket);

/**
 * @brief Release a consumed or skipped slot, returning it to EMPTY and advancing sequence.
 *
 * @param slot Slot descriptor.
 * @param ticket Ticket identifier.
 * @param capacity Total ring buffer capacity (advances sequence by capacity).
 */
RBIPC_LEAF
void rbipc_slot_release(rbipc_slot_t *slot, uint32_t ticket, uint32_t capacity);

#endif /* RBIPC_SLOT_H */
