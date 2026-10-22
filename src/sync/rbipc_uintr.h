/**
 * @file rbipc_uintr.h
 * @brief User-space inter-processor hardware interrupt (UINTR / SENDUIPI) interfaces.
 *
 * @details Implements direct core-to-core hardware signaling in Ring 3 without kernel
 *          entry or scheduler locks, achieving sub-100ns quiescent thread awakening.
 *
 * @author Seintian
 * @date 2026
 * @copyright Apache License 2.0
 */

#ifndef RBIPC_UINTR_H
#define RBIPC_UINTR_H

#include "rbipc.h"
#include "rbipc_attr.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif



/**
 * @brief Executes unprivileged SENDUIPI instruction to send a user interrupt to a target vector.
 *
 * @param[in] uirrv User interrupt target vector index.
 */
static inline void rbipc_senduipi(uint64_t uirrv) {
#if defined(__x86_64__)
  __asm__ volatile(".byte 0xf3, 0x0f, 0xc7, 0xf0" : : "a"(uirrv) : "memory");
#else
  (void)uirrv;
#endif
}

/**
 * @brief Sends a direct hardware core-to-core interrupt signal to the target peer.
 *
 * @param[in] target_vector Target user interrupt vector.
 * @return True if interrupt was delivered in hardware, false if fallback is required.
 */
RBIPC_INTERNAL bool rbipc_uintr_signal_peer(uint64_t target_vector);

#ifdef __cplusplus
}
#endif

#endif /* RBIPC_UINTR_H */
