/**
 * @file rbipc_dma.c
 * @brief Implementation of hardware-assisted asynchronous DMA offload engine (Intel DSA).
 *
 * @details Probes Intel DSA capabilities, dispatches descriptors via ENQCMD/MOVDIR64B
 *          for large payloads (>= 16 KiB), and falls back seamlessly to AVX-512/AVX2
 *          non-temporal streaming stores on systems without DSA hardware.
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
#include "rbipc_dma.h"
#include "rbipc_stream.h"
#include "rbipc_arch.h"
#include "rbipc_sync.h"

#include <fcntl.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(__x86_64__)
#include <cpuid.h>
#endif

/**
 * @brief Checks if host hardware supports Intel Data Streaming Accelerator (DSA).
 * @details Cached capability probe. _Atomic since rbipc_dma_available() may be
 *          invoked concurrently by multiple threads; the idempotent write (1 or 0)
 *          converges so relaxed ordering is sufficient.
 */
bool rbipc_dma_available(void) {
#if defined(__x86_64__)
  static _Atomic int cached = -1;
  const int cd = atomic_load_explicit(&cached, memory_order_relaxed);
  if (cd != -1) {
    return cd == 1;
  }

  unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
  int supported = 0;
  if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) {
    /* ECX bit 27: MOVDIR64B */
    /* ECX bit 29: ENQCMD */
    if ((ecx & (1U << 27)) && (ecx & (1U << 29))) {
      /* Verify that /dev/dsa work queues exist */
      if (access("/dev/dsa", F_OK) == 0) {
        supported = 1;
      }
    }
  }

  atomic_store_explicit(&cached, supported, memory_order_relaxed);
  return supported == 1;
#else
  return false;
#endif
}

/**
 * @brief Lazily-initialized DSA shared work-queue context.
 * @details Holds the open /dev/dsa work-queue descriptor, the mmap'd MMIO
 *          submission portal, and a per-context completion record. The context
 *          is initialized once on first use and cached; a failed probe leaves
 *          it permanently disabled so the streaming fallback is used.
 */
typedef struct rbipc_dsa_wq {
  int fd;                            /**< Open /dev/dsa work-queue fd, or -1 */
  void *portal;                      /**< mmap'd WQ submission portal, or NULL */
  rbipc_dma_completion_t completion; /**< Completion record written by hardware */
} rbipc_dsa_wq_t;

static rbipc_dsa_wq_t g_dsa_wq = {.fd = -1, .portal = NULL, .completion = {0}};
static _Atomic int g_dsa_wq_state = 0; /* 0 = uninitialized, 1 = ready, -1 = failed */

/**
 * @brief Initialize the DSA work-queue context.
 * @details Opens the first available /dev/dsa work queue and mmaps its MMIO
 *          submission portal. Returns 0 on success, non-zero on any failure
 *          (no device, open/mmap error).
 */
static int rbipc_dsa_wq_init(rbipc_dsa_wq_t *wq) {
  /* Locate the first work-queue device node. The accel-config driver exposes
   * one character device per configured work queue under /dev/dsa/. */
  const char *wq_path = "/dev/dsa/wq0.0";
  int fd = open(wq_path, O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    return -1;
  }

  /* The submission portal is the WQ's MMIO region, exposed via mmap on the
   * device fd. The portal size is one page. */
  void *portal = mmap(NULL, 4096U, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (portal == MAP_FAILED) {
    close(fd);
    return -1;
  }

  wq->fd = fd;
  wq->portal = portal;
  memset(&wq->completion, 0, sizeof(wq->completion));
  return 0;
}

/**
 * @brief Get the lazily-initialized DSA work-queue context.
 * @return Pointer to the ready context, or NULL if DSA is unavailable/failed.
 */
static rbipc_dsa_wq_t *rbipc_dsa_wq_get(void) {
  const int state = atomic_load_explicit(&g_dsa_wq_state, memory_order_acquire);
  if (state == 1) {
    return &g_dsa_wq;
  }
  if (state == -1) {
    return NULL;
  }

  /* First caller initializes; others wait for the result. */
  int expected = 0;
  if (atomic_compare_exchange_strong_explicit(&g_dsa_wq_state, &expected, 1,
                                              memory_order_acq_rel,
                                              memory_order_acquire)) {
    if (rbipc_dsa_wq_init(&g_dsa_wq) != 0) {
      atomic_store_explicit(&g_dsa_wq_state, -1, memory_order_release);
      return NULL;
    }
    return &g_dsa_wq;
  }

  /* Another thread is initializing; wait for the result. The initializer
   * publishes either 1 (ready) or -1 (failed), so the spin terminates. A
   * bounded micro-pause reduces cache-line contention while polling. */
  int result;
  for (uint32_t spin = 0;
       (result = atomic_load_explicit(&g_dsa_wq_state, memory_order_acquire)) == 0 &&
       spin < RBIPC_SPIN_COUNT_DEFAULT;
       ++spin) {
    rbipc_cpu_pause();
  }
  /* Branchless selection: result is 1 (ready) or -1 (failed). */
  static rbipc_dsa_wq_t *const k_result[2] = {NULL, &g_dsa_wq};
  return k_result[result == 1];
}

/**
 * @brief Build a DSA MEMMOVE descriptor and dispatch it via MOVDIR64B.
 * @details Populates a 64-byte @ref rbipc_dma_desc_t, submits it to the WQ
 *          portal, and polls the completion record until the hardware reports
 *          completion. Returns 0 on success, non-zero on dispatch or
 *          completion failure.
 * @param[in] wq   Initialized DSA work-queue context.
 * @param[in] dst  Destination buffer in shared memory.
 * @param[in] src  Source buffer.
 * @param[in] len  Byte count to transfer (must be > 0).
 * @return 0 on success, non-zero on failure.
 */
static int rbipc_dma_dispatch(rbipc_dsa_wq_t *wq, void *dst, const void *src,
                              size_t len) {
  rbipc_dma_desc_t desc;
  memset(&desc, 0, sizeof(desc));

  /* MEMMOVE opcode (0x03). Request completion record (RCR) so the hardware
   * writes the completion record on completion. */
  desc.op = 0x03U;
  desc.flags = 0x0008U; /* IDXD_OP_FLAG_RCR */
  desc.completion_addr = (uint64_t)(uintptr_t)&wq->completion;
  desc.src_addr = (uint64_t)(uintptr_t)src;
  desc.dst_addr = (uint64_t)(uintptr_t)dst;
  desc.xfer_size = (uint32_t)len;

  /* Reset the completion record before dispatch. */
  wq->completion.status = 0U;

  if (rbipc_dma_movdir64b(wq->portal, &desc) != 0) {
    return -1; /* WQ full; caller falls back to streaming */
  }

  /* Poll the completion record. The hardware writes a non-zero status byte
   * when the operation completes. */
  while (wq->completion.status == 0U) {
  }

  return (wq->completion.status == RBIPC_DMA_COMPLETION_STATUS_SUCCESS) ? 0 : -1;
}

/**
 * @brief Reserve a slot, copy the payload, and commit the write.
 * @details Shared by the blocking and bounded-timeout DMA write paths. The
 *          reserve callback abstracts the timeout variant; the payload is
 *          copied with the hardware DMA pipeline when DSA is available, else
 *          the non-temporal streaming pipeline.
 */
typedef int (*rbipc_dma_reserve_fn)(rbipc_ring_t *ring, uint32_t len,
                                    uint64_t timeout_ns, void **out_buf,
                                    uint64_t *out_ticket);

static int rbipc_dma_reserve_copy_commit(rbipc_ring_t *ring, const void *buf,
                                         size_t len, uint64_t timeout_ns,
                                         rbipc_dma_reserve_fn reserve) {
  void *slot_buf = NULL;
  uint64_t ticket = 0;
  int rc = reserve(ring, (uint32_t)len, timeout_ns, &slot_buf, &ticket);
  if (RBIPC_UNLIKELY(rc != RBIPC_OK)) {
    return rc;
  }

  /* Attempt hardware DMA dispatch when DSA is available; fall back to the
   * streaming pipeline on any failure (no WQ, dispatch error, completion
   * error). */
  rbipc_dsa_wq_t *wq = rbipc_dsa_wq_get();
  if (RBIPC_LIKELY(wq != NULL)) {
    if (rbipc_dma_dispatch(wq, slot_buf, buf, len) == 0) {
      return rbipc_commit_write(ring, ticket, (uint32_t)len);
    }
  }
  rbipc_stream_copy_auto(slot_buf, buf, len);
  return rbipc_commit_write(ring, ticket, (uint32_t)len);
}

/**
 * @brief Performs asynchronous hardware DMA payload write via Intel DSA (or streaming fallback).
 */
int rbipc_write_dma(rbipc_ring_t *ring, const void *buf, size_t len) {
  if (RBIPC_UNLIKELY(!ring || !buf || len == 0 || len > UINT32_MAX)) {
    return RBIPC_ERR_INVAL;
  }

  /* When DSA is unavailable or payload is small, dispatch to vector streaming pipeline */
  if (!rbipc_dma_available() || len < RBIPC_DMA_OFFLOAD_THRESHOLD) {
    uint64_t stream_ticket = 0;
    return rbipc_write_stream(ring, buf, (uint32_t)len, &stream_ticket);
  }

  return rbipc_dma_reserve_copy_commit(ring, buf, len, UINT64_MAX,
                                       rbipc_reserve_write_timeout);
}

/**
 * @brief Performs bounded-timeout asynchronous hardware DMA payload write.
 */
int rbipc_write_dma_timeout(rbipc_ring_t *ring, const void *buf, size_t len,
                            uint64_t timeout_ns) {
  if (RBIPC_UNLIKELY(!ring || !buf || len == 0 || len > UINT32_MAX)) {
    return RBIPC_ERR_INVAL;
  }

  if (!rbipc_dma_available() || len < RBIPC_DMA_OFFLOAD_THRESHOLD) {
    uint64_t stream_ticket = 0;
    return rbipc_write_stream_timeout(ring, buf, (uint32_t)len, timeout_ns, &stream_ticket);
  }

  return rbipc_dma_reserve_copy_commit(ring, buf, len, timeout_ns,
                                       rbipc_reserve_write_timeout);
}
