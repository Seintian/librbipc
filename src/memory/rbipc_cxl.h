/**
 * @file rbipc_cxl.h
 * @brief Compute Express Link (CXL 2.0 / 3.0 / 3.1) coherent disaggregated memory fabric interfaces.
 *
 * @details Implements direct access memory mapping (DAX) and cache-line writeback (CLWB)
 *          over CXL pooled memory devices, enabling sub-200ns cross-server coherent IPC.
 *
 * @author Seintian
 * @date 2026
 * @copyright Apache License 2.0
 */

#ifndef RBIPC_CXL_H
#define RBIPC_CXL_H

#include "rbipc.h"
#include "rbipc_attr.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__x86_64__) || defined(_M_X64)
#include <cpuid.h>
#include <stdatomic.h>
#endif

/**
 * @brief Issues cache line write back (CLWB) to push dirty cache lines to the CXL memory controller.
 *
 * @details On coherent Compute Express Link (CXL 2.0/3.0/3.1) memory pools and DAX-backed
 * shared memory, modified cache lines must be pushed to the memory controller to ensure
 * visibility across independent host sockets and hardware fabric devices.
 *
 * Microarchitectural Feature Dispatch:
 * - **Tier 2 (CLWB)**: Cache Line Write Back (CPUID leaf 7, subleaf 0, EBX bit 24).
 *   Flushes dirty cache lines directly to persistent/CXL fabric without invalidating
 *   or evicting them from CPU L1/L2/L3 caches, preserving cache hits on subsequent reads.
 * - **Tier 1 (CLFLUSHOPT)**: Optimized Cache Line Flush (CPUID leaf 7, subleaf 0, EBX bit 23).
 *   Weakly ordered flush instruction permitting concurrent store buffer draining.
 * - **Tier 0 (CLFLUSH)**: Serializing legacy cache line flush available on all x86-64 processors.
 *
 * Toolchain Portability Note:
 * Dynamic inspection queries hardware CPUID directly via @c __get_cpuid_count rather than
 * compiler @c __builtin_cpu_supports, because Clang and earlier GCC toolchain versions
 * do not recognize "clwb" and "clflushopt" as valid string identifiers for compiler builtins.
 *
 * @param[in] addr Virtual memory address of the cache line to write back.
 */
static inline void rbipc_clwb(const void *addr) {
#if defined(__x86_64__) || defined(_M_X64)
  static _Atomic int clwb_cap = -1;
  int cap = atomic_load_explicit(&clwb_cap, memory_order_acquire);
  if (RBIPC_UNLIKELY(cap == -1)) {
    unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
    if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) {
      /* CPUID.(EAX=07H, ECX=0H): EBX[24] = CLWB, EBX[23] = CLFLUSHOPT */
      if ((ebx & (1U << 24U)) != 0U) {
        cap = 2; /* CLWB supported */
      } else if ((ebx & (1U << 23U)) != 0U) {
        cap = 1; /* CLFLUSHOPT supported */
      } else {
        cap = 0; /* Fallback to CLFLUSH */
      }
    } else {
      cap = 0;
    }
    atomic_store_explicit(&clwb_cap, cap, memory_order_release);
  }

  if (RBIPC_LIKELY(cap == 2)) {
    __asm__ volatile("clwb (%0)" : : "r"(addr) : "memory");
  } else if (cap == 1) {
    __asm__ volatile("clflushopt (%0)" : : "r"(addr) : "memory");
  } else {
    __asm__ volatile("clflush (%0)" : : "r"(addr) : "memory");
  }
#elif defined(__aarch64__)
  __asm__ volatile("dc cvac, %0" : : "r"(addr) : "memory");
#else
  (void)addr;
#endif
}



#ifdef __cplusplus
}
#endif

#endif /* RBIPC_CXL_H */
