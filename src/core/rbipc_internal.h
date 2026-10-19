/**
 * @file rbipc_internal.h
 * @brief Internal runtime handle definitions, bounded string utilities, and
 * handle invariants.
 *
 * @details Defines the concrete representation of the opaque @ref rbipc_ring_t
 * type, encapsulates defensive bounded memory copying, and provides internal
 * invariant validators.
 *
 * @author Seintian
 * @date 2026
 * @copyright Apache License 2.0
 */

#ifndef RBIPC_INTERNAL_H
#define RBIPC_INTERNAL_H

#include "rbipc.h"
#include "rbipc_attr.h"
#include "rbipc_predicate.h"
#include "rbipc_reactor.h"

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/**
 * @struct rbipc_ring
 * @brief Concrete process-local runtime handle for an attached ring buffer
 * instance.
 * @details Manages virtual address space mappings, file descriptors, and cached
 * metadata for a specific process attachment. Created via @ref rbipc_create or
 * @ref rbipc_attach.
 */
struct rbipc_ring {
  int fd; /**< Open file descriptor referencing the underlying shared memory
             object. */
  char name[NAME_MAX + 1]; /**< POSIX shared memory name (empty string if
                              anonymous memfd). */

  rbipc_shm_header_t *hdr; /**< Direct virtual address pointer to shared memory
                              control header. */
  rbipc_slot_t *slots;  /**< Direct virtual address pointer to slot descriptor
                           control array. */
  void *ctrl_map;       /**< Base virtual memory address of the metadata mapping
                           (header + slots). */
  size_t ctrl_map_size; /**< Exact byte size of the mapped metadata region
                           (page-aligned). */
  void *data_map;   /**< Base virtual memory address of the contiguous shared
                       memory payload buffer (1x linear for slot rings, 2x for
                       stream mirrors). */
  size_t data_size; /**< Byte size of a single circular data buffer (half of
                       @ref data_map reservation). */
  uint32_t roles;   /**< Bitmask of participant role flags (PRODUCER, CONSUMER,
                       MONITOR). */
  bool is_creator;  /**< True if this process created the ring buffer; false if
                       attached. */
  pid_t cached_pid; /**< Cached process PID to avoid redundant getpid() system
                       calls during reservations. */
  uint32_t wait_policy; /**< Wait and backoff synchronization policy
                           (RBIPC_WAIT_ADAPTIVE (default), RBIPC_WAIT_ZERO_SPIN,
                            or RBIPC_WAIT_HW_MONITOR). */
  int event_fd; /**< Bound eventfd descriptor for epoll reactor integration (-1
                   if disabled). */
  bool owns_eventfd; /**< True if this ring handle created the eventfd and
                        should close it on detach. */
  bool is_trusted; /**< True if ring operates in Tier 2 trusted mode (pure Vyukov sequence turns). */
  bool is_packed;  /**< True if ring operates in Tier 3 64-bit packed descriptor mode. */
};

/**
 * @brief Dispatch notification to bound eventfd if reactor waiters are present.
 */
RBIPC_INLINE void rbipc_ring_signal_eventfd(const rbipc_ring_t *ring) {
  rbipc_reactor_signal(ring);
}

/**
 * @brief Bounded, null-terminating string copy without insecure C runtime
 * warnings.
 *
 * @details Copies characters from @p src to @p dest up to dest_size - 1 bytes
 * and guarantees NUL-termination. Defensively handles NULL pointers and
 * zero-length destination buffers.
 *
 * @param[out] dest      Destination character buffer.
 * @param[in]  dest_size Total byte capacity of destination buffer.
 * @param[in]  src       Source NUL-terminated string.
 */
RBIPC_INLINE void rbipc_str_copy(char *RBIPC_RESTRICT dest, size_t dest_size,
                                 const char *RBIPC_RESTRICT src) {
  if (RBIPC_UNLIKELY(!dest || dest_size == 0)) {
    return;
  }
  if (RBIPC_UNLIKELY(!src)) {
    dest[0] = '\0';
    return;
  }
  size_t i = 0;
  while (i + 1 < dest_size && src[i] != '\0') {
    dest[i] = src[i];
    i++;
  }
  dest[i] = '\0';
}

/**
 * @brief Evaluate whether an allocated ring handle satisfies all operational
 * invariants.
 *
 * @param[in] ring Pointer to ring handle to validate.
 * @return true if non-null, all memory regions are mapped, and header matches
 * protocol invariants; false otherwise.
 */
RBIPC_INLINE RBIPC_PURE bool rbipc_ring_is_valid(const rbipc_ring_t *ring) {
  if (RBIPC_UNLIKELY(!ring))
    return false;
  if (RBIPC_UNLIKELY(!ring->hdr || !ring->slots || !ring->ctrl_map ||
                     !ring->data_map))
    return false;
  return rbipc_header_is_valid(ring->hdr);
}

#endif /* RBIPC_INTERNAL_H */