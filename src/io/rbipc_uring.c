/**
 * @file rbipc_uring.c
 * @brief Implementation of unified Linux io_uring asynchronous kernel-bypass zero-copy integration.
 *
 * @details Directly invokes Linux io_uring system calls (io_uring_setup, io_uring_register)
 *          without third-party library dependencies, registering librbipc shared memory buffers
 *          for zero-copy high-throughput network and storage ingestion.
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
#include "rbipc_uring.h"
#include "rbipc_arch.h"

#include <errno.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

#ifndef SYS_io_uring_setup
#if defined(__NR_io_uring_setup)
#define SYS_io_uring_setup __NR_io_uring_setup
#define SYS_io_uring_enter __NR_io_uring_enter
#define SYS_io_uring_register __NR_io_uring_register
#elif defined(__mips__)
#define SYS_io_uring_setup 4425
#define SYS_io_uring_enter 4426
#define SYS_io_uring_register 4427
#elif defined(__alpha__)
#define SYS_io_uring_setup 535
#define SYS_io_uring_enter 536
#define SYS_io_uring_register 537
#elif defined(__x86_64__) || defined(__aarch64__) || defined(__riscv) ||        \
    defined(__powerpc__) || defined(__s390x__)
#define SYS_io_uring_setup 425
#define SYS_io_uring_enter 426
#define SYS_io_uring_register 427
#endif
#endif

#ifndef IORING_REGISTER_BUFFERS
#define IORING_REGISTER_BUFFERS 0U
#endif

#ifndef IORING_SETUP_SQPOLL
#define IORING_SETUP_SQPOLL (1U << 1)
#endif

/* Linux kernel struct io_sqring_offsets */
struct rbipc_sqring_offsets {
  uint32_t head;
  uint32_t tail;
  uint32_t ring_mask;
  uint32_t ring_entries;
  uint32_t flags;
  uint32_t dropped;
  uint32_t array;
  uint32_t resv1;
  uint64_t user_addr;
};

/* Linux kernel struct io_cqring_offsets */
struct rbipc_cqring_offsets {
  uint32_t head;
  uint32_t tail;
  uint32_t ring_mask;
  uint32_t ring_entries;
  uint32_t overflow;
  uint32_t cqes;
  uint32_t flags;
  uint32_t resv1;
  uint64_t user_addr;
};

/* Linux kernel struct io_uring_params */
struct rbipc_io_uring_params {
  uint32_t sq_entries;
  uint32_t cq_entries;
  uint32_t flags;
  uint32_t sq_thread_cpu;
  uint32_t sq_thread_idle;
  uint32_t features;
  uint32_t wq_fd;
  uint32_t resv[3];
  struct rbipc_sqring_offsets sq_off;
  struct rbipc_cqring_offsets cq_off;
};

/**
 * @brief Checks if the Linux io_uring subsystem is supported by host kernel.
 */
bool rbipc_uring_available(void) {
#ifdef SYS_io_uring_setup
  /*
   * Cached capability probe. _Atomic since rbipc_uring_available() may be
   * invoked concurrently by multiple threads; the idempotent write (1 or 0)
   * converges so relaxed ordering is sufficient.
   */
  static _Atomic int cached = -1;
  const int cd = atomic_load_explicit(&cached, memory_order_relaxed);
  if (cd != -1) {
    return cd == 1;
  }

  struct rbipc_io_uring_params p = {0};
  long fd = syscall(SYS_io_uring_setup, 2, &p);
  bool supported = (fd >= 0);
  if (supported) {
    close((int)fd);
  }

  atomic_store_explicit(&cached, supported ? 1 : 0, memory_order_relaxed);
  return supported;
#else
  return false;
#endif
}

/**
 * @brief Pre-registers ring buffer memory slices with an existing Linux io_uring instance.
 */
int rbipc_uring_register_ring(const rbipc_ring_t *ring, int uring_fd) {
#ifdef SYS_io_uring_register
  if (RBIPC_UNLIKELY(!ring || uring_fd < 0 || !ring->data_map || ring->data_size == 0)) {
    return RBIPC_ERR_INVAL;
  }

  const long page = (long)rbipc_page_size();
  if (RBIPC_UNLIKELY(page <= 0 ||
                     (uintptr_t)ring->data_map % (uintptr_t)page != 0 ||
                     ring->data_size % (uintptr_t)page != 0 ||
                     ring->data_size > UINT32_MAX)) {
    /*
     * io_uring fixed-buffer registration requires page-aligned addresses and
     * page-aligned lengths (and IOV bufs are u32-sized). Rejecting early gives
     * a clear diagnostic instead of a confusing EINVAL from the kernel.
     */
    return RBIPC_ERR_INVAL;
  }

  struct iovec iov = {
      .iov_base = ring->data_map,
      .iov_len = ring->data_size,
  };

  long ret = syscall(SYS_io_uring_register, uring_fd, IORING_REGISTER_BUFFERS,
                     &iov, 1);
  if (ret < 0) {
    return RBIPC_ERR_SYS;
  }

  return RBIPC_OK;
#else
  (void)ring;
  (void)uring_fd;
  return RBIPC_ERR_NOTSUP;
#endif
}

/**
 * @brief Creates an optimized io_uring instance pre-bound to the ring's fixed memory buffers.
 */
int rbipc_uring_create(const rbipc_ring_t *ring, unsigned int entries,
                       uint32_t flags, int *out_uring_fd) {
#ifdef SYS_io_uring_setup
  if (RBIPC_UNLIKELY(!out_uring_fd || entries == 0)) {
    return RBIPC_ERR_INVAL;
  }

  struct rbipc_io_uring_params p = {0};
  p.flags = flags;

  long fd = syscall(SYS_io_uring_setup, entries, &p);
  if (fd < 0 && (flags & IORING_SETUP_SQPOLL) && (errno == EPERM || errno == EINVAL)) {
    /* Fallback without SQPOLL if unprivileged */
    p.flags &= ~IORING_SETUP_SQPOLL;
    fd = syscall(SYS_io_uring_setup, entries, &p);
  }

  if (fd < 0) {
    return RBIPC_ERR_SYS;
  }

  int uring_fd = (int)fd;
  if (ring) {
    int rc = rbipc_uring_register_ring(ring, uring_fd);
    if (rc != RBIPC_OK) {
      close(uring_fd);
      return rc;
    }
  }

  *out_uring_fd = uring_fd;
  return RBIPC_OK;
#else
  (void)ring;
  (void)entries;
  (void)flags;
  (void)out_uring_fd;
  return RBIPC_ERR_NOTSUP;
#endif
}

/**
 * @brief Closes an io_uring instance created via rbipc_uring_create().
 */
void rbipc_uring_destroy(int uring_fd) {
  if (uring_fd >= 0) {
    close(uring_fd);
  }
}
