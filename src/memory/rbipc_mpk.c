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
#include "rbipc_arch.h"

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
  /*
   * Cached capability probe. _Atomic since rbipc_mpk_available() may be
   * invoked concurrently by multiple threads; the idempotent write (1 or 0)
   * converges so relaxed ordering is sufficient.
   */
  static _Atomic int cached = -1;
  const int cd = atomic_load_explicit(&cached, memory_order_relaxed);
  if (cd != -1) {
    return cd == 1;
  }

  unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
  bool supported = false;
  if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) {
    /* ECX bit 3: PKU (Protection Keys User pages) */
    /* ECX bit 4: OSPKE (OS has enabled CR4.PKE) */
    if ((ecx & RBIPC_CPUID_ECX_PKU_BIT) &&
        (ecx & RBIPC_CPUID_ECX_OSPKE_BIT)) {
      /* Verify kernel runtime support via pkey_alloc */
      int pkey = pkey_alloc(0, 0);
      if (pkey >= 0) {
        pkey_free(pkey);
        supported = true;
      }
    }
  }

  atomic_store_explicit(&cached, supported ? 1 : 0, memory_order_relaxed);
  return supported;
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

  /*
   * Permanently seal virtual memory mappings (Linux 6.10+ mseal).
   *
   * mseal(2) is irreversible — it prevents later munmap()/mprotect() on the
   * sealed VMAs. To avoid a per-detach virtual address leak and a silent
   * munmap() failure on teardown, we record the fact that the regions were
   * actually sealed in ring->is_sealed. Teardown then deliberately skips the
   * impossible munmap(), releasing only the ring handle (and MPK keys); the
   * sealed address range remains reserved for the process lifetime by design.
   *
   * rbipc_vmem_seal() returns RBIPC_OK even when mseal is unavailable (it
   * becomes a no-op), so is_sealed is only raised when the kernel actually
   * supports and applied the seal. This keeps teardown munmap() working for
   * the unsealed (no-mseal) case and avoids leaking real VMAs.
   */
  if (rbipc_mseal_available() && ring->ctrl_map && ring->ctrl_map_size > 0 &&
      ring->data_map && ring->data_size > 0) {
    int seal_ctrl = rbipc_vmem_seal(ring->ctrl_map, ring->ctrl_map_size);
    if (RBIPC_UNLIKELY(seal_ctrl != RBIPC_OK)) {
      return seal_ctrl;
    }
    int seal_data = rbipc_vmem_seal(ring->data_map, ring->data_size);
    if (RBIPC_UNLIKELY(seal_data != RBIPC_OK)) {
      return seal_data;
    }
    ring->is_sealed = true;
  } else {
    ring->is_sealed = false;
  }

  return RBIPC_OK;
}

/**
 * @brief Releases a hardware protection key if it was allocated by this ring.
 * @details Resets the key to -1 after freeing so it is never released twice.
 */
RBIPC_INLINE void rbipc_mpk_free_key(int *pkey, bool is_creator) {
  if (*pkey > 0 && is_creator) {
    pkey_free(*pkey);
    *pkey = -1;
  }
}

/**
 * @brief Releases hardware protection keys allocated for an isolated ring.
 */
void rbipc_mpk_destroy(rbipc_ring_t *ring) {
  if (!ring) {
    return;
  }
  rbipc_mpk_free_key(&ring->data_pkey, ring->is_creator);
  rbipc_mpk_free_key(&ring->ctrl_pkey, ring->is_creator);
}

/*
 * WRPKRU semantics: the PKRU register is per-thread state, so the allow/deny
 * write window only gates the calling thread's own accesses, not a
 * cross-process protection boundary. The allow/deny pair is side-effect-neutral
 * with respect to the enclosing thread's pre-existing PKU context: the original
 * PKRU is captured on the first allow_write() and restored on the matching
 * deny_write(). This prevents the window from permanently lifting (or
 * clobbering) write-deny bits that an enclosing MPK-aware application had set
 * for other protection keys.
 *
 * Constraint: a given thread must not hold multiple concurrent write-reservation
 * windows (i.e. allow_write() then deny_write()) on the same ring without
 * interleaving another ring's window; in that single-window-per-thread model
 * the save/restore pair is exact.
 */
static _Thread_local uint32_t s_mpk_saved_pkru = 0U;
static _Thread_local bool    s_mpk_window_open = false;

/**
 * @brief Temporarily enables write permissions to the data ring via WRPKRU.
 */
void rbipc_mpk_allow_write(const rbipc_ring_t *ring) {
  if (!ring || ring->data_pkey <= 0) {
    return;
  }

  if (RBIPC_UNLIKELY(!s_mpk_window_open)) {
    /* First window on this thread: snapshot the enclosing PKU context. */
    s_mpk_saved_pkru = rbipc_rdpkru();
    s_mpk_window_open = true;
  }
  /* Clear bit (2 * pkey + 1) -> Disable Write cleared for the data key only. */
  uint32_t pkru =
      s_mpk_saved_pkru & ~RBIPC_PKRU_WRITE_BIT(ring->data_pkey);
  rbipc_wrpkru(pkru);
}

/**
 * @brief Disables write permissions to the data ring via WRPKRU.
 */
void rbipc_mpk_deny_write(const rbipc_ring_t *ring) {
  if (!ring || ring->data_pkey <= 0) {
    return;
  }

  if (RBIPC_LIKELY(s_mpk_window_open)) {
    /* Restore the exact PKU context captured by allow_write(). */
    rbipc_wrpkru(s_mpk_saved_pkru);
    s_mpk_window_open = false;
  }
}
