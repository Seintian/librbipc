/**
 * @file rbipc_audit.h
 * @brief Peer liveness auditing, dead-peer crash recovery, ticket-hole healing,
 * and lease management.
 *
 * @details Implements Linux pidfd and POSIX signal process auditing to detect
 * crashed peers, executes atomic slot poisoning for terminated producers,
 * recovers slots abandoned by dead consumers, heals orphaned ticket allocations
 * (ticket-holes), and audits/revokes expired consumer leases.
 *
 * References:
 * - Christian Santarelli (2026), "librbipc: Lock-Free Shared-Memory Ring Buffer
 * IPC".
 * - Linux kernel pidfd_open(2) safe process recycling auditing.
 *
 * @author Seintian
 * @date 2026
 * @copyright Apache License 2.0
 */

#ifndef RBIPC_AUDIT_H
#define RBIPC_AUDIT_H

#include "rbipc.h"
#include "rbipc_attr.h"
#include "rbipc_sync.h"

#include <stdbool.h>
#include <sys/types.h>

/**
 * @def RBIPC_TICKET_HOLE_GRACE_NS
 * @brief Default 50ms grace duration before an unpopulated EMPTY ticket-hole is
 * declared orphaned.
 */
#define RBIPC_TICKET_HOLE_GRACE_NS 50000000ULL

/**
 * @brief Audit whether a peer process with the specified PID is currently
 * alive.
 *
 * @details Probes process existence via Linux pidfd_open(2) and poll(2) with
 * fallback to kill(pid, 0).
 *
 * @param[in] pid Process identifier to audit.
 * @return true if process exists; false if confirmed terminated (ESRCH).
 */
RBIPC_NODISCARD RBIPC_LEAF bool rbipc_audit_is_peer_alive(pid_t pid);

/**
 * @brief Inspect slot for dead producer crash and poison if producer
 * terminated.
 *
 * @param[in,out] hdr    Shared memory control header.
 * @param[in,out] slot   Slot descriptor to inspect.
 * @param[in]     ticket Reserving ticket number.
 *
 * @return true if slot was recovered and poisoned; false otherwise.
 */
RBIPC_LEAF
bool rbipc_audit_recover_dead_producer(rbipc_shm_header_t *hdr,
                                       rbipc_slot_t *slot, uint64_t ticket);

/**
 * @brief Audit a contiguous range of slots for dead producer crashes.
 *
 * @param[in,out] hdr   Shared memory control header.
 * @param[in,out] slots Slot descriptor array.
 * @param[in]     t     Starting ticket.
 * @param[in]     count Number of slots to audit.
 */
RBIPC_LEAF
void rbipc_audit_recover_dead_producer_range(rbipc_shm_header_t *hdr,
                                             rbipc_slot_t *slots, uint64_t t,
                                             uint32_t count);

/**
 * @brief Inspect slot for dead consumer crash or expired lease and recycle
 * slot.
 *
 * @param[in,out] hdr    Shared memory control header.
 * @param[in,out] slot   Slot descriptor to inspect.
 * @param[in]     ticket Target vacant ticket.
 *
 * @return true if slot was recovered; false otherwise.
 */
RBIPC_LEAF
bool rbipc_audit_recover_dead_consumer(rbipc_shm_header_t *hdr,
                                       rbipc_slot_t *slot, uint64_t ticket);

/**
 * @brief Audit a contiguous range of slots for dead consumer crashes.
 *
 * @param[in,out] hdr   Shared memory control header.
 * @param[in,out] slots Slot descriptor array.
 * @param[in]     t     Starting ticket.
 * @param[in]     count Number of slots to audit.
 */
RBIPC_LEAF
void rbipc_audit_recover_dead_consumer_range(rbipc_shm_header_t *hdr,
                                             rbipc_slot_t *slots, uint64_t t,
                                             uint32_t count);

/**
 * @brief Detect and heal orphaned ticket allocations (ticket holes) where
 * producer crashed before reserving.
 *
 * @param[in,out] hdr        Shared memory control header.
 * @param[in,out] slot       Slot descriptor at the observed ticket.
 * @param[in]     ticket     Observed ticket number.
 * @param[in,out] sync_state Thread-local synchronization state tracking
 * observation timestamp.
 *
 * @return true if ticket hole was diagnosed and healed via poisoning; false
 * otherwise.
 */
RBIPC_LEAF
bool rbipc_audit_check_and_heal_ticket_hole(rbipc_shm_header_t *hdr,
                                            rbipc_slot_t *slot, uint64_t ticket,
                                            rbipc_sync_state_t *sync_state);

/**
 * @brief Proactively audit entire ring slot table and revoke all expired
 * consumer leases.
 *
 * @param[in,out] hdr   Shared memory control header.
 * @param[in,out] slots Slot descriptor array.
 *
 * @return Number of expired leases successfully revoked and recycled.
 */
RBIPC_LEAF
int rbipc_audit_revoke_expired_leases(rbipc_shm_header_t *hdr,
                                      rbipc_slot_t *slots);

#endif /* RBIPC_AUDIT_H */