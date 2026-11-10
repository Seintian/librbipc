/**
 * @file rbipc_uintr.c
 * @brief Implementation of user-space inter-processor hardware interrupts
 * (UINTR).
 *
 * @details Detects CPUID UINTR support and issues direct SENDUIPI hardware
 * signaling for sub-100ns consumer wakeup in the hybrid progressive wait
 * ladder.
 *
 * @author Seintian
 * @date 2026
 * @copyright Apache License 2.0
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "rbipc.h"
#include "rbipc_uintr.h"
#include "rbipc_arch.h"

#if defined(__x86_64__)
#include <cpuid.h>
#endif

/**
 * @brief Checks if host CPU and OS kernel support Intel User Interrupts (UINTR
 * / SENDUIPI).
 */
bool rbipc_uintr_available(void) {
#if defined(__x86_64__)
    /*
     * Cached capability probe. _Atomic since rbipc_uintr_available() may be
     * invoked concurrently by multiple threads; the write is idempotent (always
     * converges to 1 or 0) so relaxed ordering is sufficient. The cache is
     * never reset after the first probe.
     */
    static _Atomic int cached = -1;
    const int cd = atomic_load_explicit(&cached, memory_order_relaxed);
    if (cd != -1) {
        return cd == 1;
    }

    unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
    bool supported = false;
    if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) {
        /* EDX bit 5: UINTR (User Interrupts) */
        if (edx & RBIPC_CPUID_EDX_UINTR_BIT) {
            supported = true;
        }
    }

    /* Only the first thread to probe publishes the result; others may probe
     * once more but converge on the same value (idempotent write). */
    atomic_store_explicit(&cached, supported ? 1 : 0, memory_order_relaxed);
    return supported;
#else
    return false;
#endif
}

/**
 * @brief Sends a direct hardware core-to-core interrupt signal to the target
 * peer.
 */
bool rbipc_uintr_signal_peer(uint64_t target_vector) {
    if (target_vector == 0 || !rbipc_uintr_available()) {
        return false;
    }

    rbipc_senduipi(target_vector);
    return true;
}
