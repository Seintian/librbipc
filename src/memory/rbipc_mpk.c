/**
 * @file rbipc_mpk.c
 * @brief Implementation of Intel Memory Protection Keys (MPK / PKU) hardware domain isolation.
 *
 * @details Manages hardware protection domains via pkey_alloc, pkey_mprotect,
 *          and direct WRPKRU register manipulation for zero-trust slot isolation.
 *
 * @author Seintian
 * @date 2026
 * @copyright Apache License 2.0
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "rbipc.h"
#include "rbipc_internal.h"
#include "rbipc_mpk.h"
#include "rbipc_vmem.h"

#include <errno.h>
#include <sys/mman.h>

#if defined(__x86_64__)
#include <cpuid.h>
#endif

#ifndef PKEY_DISABLE_ACCESS
#define PKEY_DISABLE_ACCESS 0x1
#endif
#ifndef PKEY_DISABLE_WRITE
#define PKEY_DISABLE_WRITE 0x2
#endif

/**
 * @brief Checks if host CPU and OS kernel support Intel Memory Protection Keys (MPK/PKU).
 */
bool rbipc_mpk_available(void) {
#if defined(__x86_64__)
  static int cached = -1;
  if (cached != -1) {
    return cached == 1;
  }

  unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
  if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) {
    /* ECX bit 3: PKU (Protection Keys User pages) */
    /* ECX bit 4: OSPKE (OS has enabled CR4.PKE) */
    if ((ecx & (1U << 3)) && (ecx & (1U << 4))) {
      /* Verify kernel runtime support via pkey_alloc */
      int pkey = pkey_alloc(0, 0);
      if (pkey >= 0) {
        pkey_free(pkey);
        cached = 1;
        return true;
      }
    }
  }

  cached = 0;
  return false;
#else
  return false;
#endif
}

/**
 * @brief Initializes hardware MPK domain protection for an isolated ring.
 */
int rbipc_mpk_init(rbipc_ring_t *ring) {
  if (RBIPC_UNLIKELY(!ring)) {
    return RBIPC_ERR_INVAL;
  }

  ring->data_pkey = -1;
  ring->ctrl_pkey = -1;

  if (rbipc_mpk_available()) {
    /* Allocate data key: write-disabled by default */
    int data_key = pkey_alloc(0, PKEY_DISABLE_WRITE);
    if (data_key >= 0) {
      int rc = pkey_mprotect(ring->data_map, ring->data_size,
                             PROT_READ | PROT_WRITE, data_key);
      if (rc == 0) {
        ring->data_pkey = data_key;
      } else {
        pkey_free(data_key);
      }
    }
  }

  /* Permanently seal virtual memory mappings (Linux 6.10+ mseal) */
  if (ring->ctrl_map && ring->ctrl_map_size > 0) {
    int seal_rc = rbipc_vmem_seal(ring->ctrl_map, ring->ctrl_map_size);
    if (RBIPC_UNLIKELY(seal_rc != RBIPC_OK)) {
      return seal_rc;
    }
  }
  if (ring->data_map && ring->data_size > 0) {
    int seal_rc = rbipc_vmem_seal(ring->data_map, ring->data_size);
    if (RBIPC_UNLIKELY(seal_rc != RBIPC_OK)) {
      return seal_rc;
    }
  }

  return RBIPC_OK;
}

/**
 * @brief Releases hardware protection keys allocated for an isolated ring.
 */
void rbipc_mpk_destroy(rbipc_ring_t *ring) {
  if (!ring) {
    return;
  }

  if (ring->data_pkey > 0 && ring->is_creator) {
    pkey_free(ring->data_pkey);
    ring->data_pkey = -1;
  }
  if (ring->ctrl_pkey > 0 && ring->is_creator) {
    pkey_free(ring->ctrl_pkey);
    ring->ctrl_pkey = -1;
  }
}

/**
 * @brief Temporarily enables write permissions to the data ring via WRPKRU.
 */
void rbipc_mpk_allow_write(const rbipc_ring_t *ring) {
  if (!ring || ring->data_pkey <= 0) {
    return;
  }

  uint32_t pkru = rbipc_rdpkru();
  /* Clear bit (2 * pkey + 1) -> Disable Write cleared */
  pkru &= ~(1U << (2 * (uint32_t)ring->data_pkey + 1));
  rbipc_wrpkru(pkru);
}

/**
 * @brief Disables write permissions to the data ring via WRPKRU.
 */
void rbipc_mpk_deny_write(const rbipc_ring_t *ring) {
  if (!ring || ring->data_pkey <= 0) {
    return;
  }

  uint32_t pkru = rbipc_rdpkru();
  /* Set bit (2 * pkey + 1) -> Disable Write set */
  pkru |= (1U << (2 * (uint32_t)ring->data_pkey + 1));
  rbipc_wrpkru(pkru);
}
