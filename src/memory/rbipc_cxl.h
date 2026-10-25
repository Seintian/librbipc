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

/**
 * @brief Issues cache line write back (CLWB) to push dirty cache lines to the CXL memory controller.
 *
 * @param[in] addr Virtual memory address of the cache line to write back.
 */
static inline void rbipc_clwb(const void *addr) {
#if defined(__x86_64__)
  static int clwb_cap = -1;
  if (RBIPC_UNLIKELY(clwb_cap == -1)) {
    if (__builtin_cpu_supports("clwb")) {
      clwb_cap = 2;
    } else if (__builtin_cpu_supports("clflushopt")) {
      clwb_cap = 1;
    } else {
      clwb_cap = 0;
    }
  }

  if (RBIPC_LIKELY(clwb_cap == 2)) {
    __asm__ volatile("clwb (%0)" : : "r"(addr) : "memory");
  } else if (clwb_cap == 1) {
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
