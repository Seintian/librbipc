/**
 * @file rbipc_simd.h
 * @brief SIMD/Vectorized sequence scanning for vacant and ready ring buffer
 * slots.
 *
 * @details Implements hardware-accelerated 256-bit AVX2 (x86_64) and 128-bit
 * NEON (ARM) vector comparison pipelines for scanning contiguous sequences of
 * slot descriptors, with portable scalar fallback.
 *
 * References:
 * - Intel 64 and IA-32 Architectures Optimization Reference Manual (AVX2
 * vectorization).
 * - ARM Architecture Reference Manual (NEON Advanced SIMD).
 *
 * @author Seintian
 * @date 2026
 * @copyright Apache License 2.0
 */

#ifndef RBIPC_SIMD_H
#define RBIPC_SIMD_H

#include "rbipc.h"
#include "rbipc_attr.h"
#include <stdint.h>

/**
 * @brief Scan up to @p count contiguous slots starting at ticket @p t for
 * vacancy.
 *
 * @details Evaluates whether slot sequences match their target reservation
 * ticket. Utilizes AVX2/NEON vector instructions when available to scan 4 slots
 * per cycle.
 *
 * @param[in] slots Array of slot descriptors in shared memory.
 * @param[in] mask  Ring capacity bitwise mask (capacity - 1).
 * @param[in] t     Starting producer write ticket.
 * @param[in] count Maximum number of slots to inspect.
 *
 * @return Number of contiguous vacant slots available (0 to @p count).
 */
RBIPC_INTERNAL RBIPC_NODISCARD RBIPC_LEAF uint32_t rbipc_simd_scan_vacant_slots(
    const rbipc_slot_t *slots, uint32_t mask, uint64_t t, uint32_t count);

/**
 * @brief Scan up to @p count contiguous slots starting at ticket @p t for read
 * readiness.
 *
 * @details Evaluates whether slot sequences match their target committed ticket
 * (ticket + 1). Utilizes AVX2/NEON vector instructions when available to scan 4
 * slots per cycle.
 *
 * @param[in] slots Array of slot descriptors in shared memory.
 * @param[in] mask  Ring capacity bitwise mask (capacity - 1).
 * @param[in] t     Starting consumer read ticket.
 * @param[in] count Maximum number of slots to inspect.
 *
 * @return Number of contiguous ready slots available (0 to @p count).
 */
RBIPC_INTERNAL RBIPC_NODISCARD RBIPC_LEAF uint32_t rbipc_simd_scan_ready_slots(
    const rbipc_slot_t *slots, uint32_t mask, uint64_t t, uint32_t count);

#endif /* RBIPC_SIMD_H */
