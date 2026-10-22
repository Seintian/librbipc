/**
 * @file rbipc_mpk.h
 * @brief Intel Memory Protection Keys (MPK / PKU) hardware domain isolation interfaces.
 *
 * @details Implements sub-microsecond user-space protection domain isolation using
 *          hardware protection keys (MPK/PKU) and the unprivileged WRPKRU instruction.
 *          Provides zero-syscall hardware memory isolation for Tier 1 Zero-Trust rings.
 *
 * @author Seintian
 * @date 2026
 * @copyright Apache License 2.0
 */

#ifndef RBIPC_MPK_H
#define RBIPC_MPK_H

#include "rbipc.h"
#include "rbipc_attr.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Read the current 32-bit PKRU register value.
 * @return Current thread PKRU register contents.
 */
static inline uint32_t rbipc_rdpkru(void) {
#if defined(__x86_64__)
  uint32_t eax = 0, edx = 0, ecx = 0;
  __asm__ volatile("rdpkru" : "=a"(eax), "=d"(edx) : "c"(ecx));
  return eax;
#else
  return 0;
#endif
}

/**
 * @brief Write a new 32-bit value to the thread-local PKRU register.
 * @param[in] pkru New PKRU register configuration.
 */
static inline void rbipc_wrpkru(uint32_t pkru) {
#if defined(__x86_64__)
  uint32_t eax = pkru;
  uint32_t edx = 0;
  uint32_t ecx = 0;
  __asm__ volatile("wrpkru" : : "a"(eax), "c"(ecx), "d"(edx));
#else
  (void)pkru;
#endif
}



/**
 * @brief Initializes hardware MPK domain protection for an isolated ring.
 *
 * @param[in,out] ring Attached ring buffer handle.
 * @return @ref RBIPC_OK on success, error code on failure.
 */
RBIPC_INTERNAL RBIPC_NODISCARD int rbipc_mpk_init(rbipc_ring_t *ring);

/**
 * @brief Releases hardware protection keys allocated for an isolated ring.
 *
 * @param[in,out] ring Attached ring buffer handle.
 */
RBIPC_INTERNAL void rbipc_mpk_destroy(rbipc_ring_t *ring);

/**
 * @brief Temporarily enables write permissions to the data ring via WRPKRU (~20 cycles).
 *
 * @param[in] ring Attached ring buffer handle.
 */
RBIPC_INTERNAL void rbipc_mpk_allow_write(const rbipc_ring_t *ring);

/**
 * @brief Disables write permissions to the data ring via WRPKRU (read-only hardware lock).
 *
 * @param[in] ring Attached ring buffer handle.
 */
RBIPC_INTERNAL void rbipc_mpk_deny_write(const rbipc_ring_t *ring);

#ifdef __cplusplus
}
#endif

#endif /* RBIPC_MPK_H */
