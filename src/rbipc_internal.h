/**
 * @file rbipc_internal.h
 * @brief Internal handle definitions, memory utilities, and private contracts
 */

#ifndef RBIPC_INTERNAL_H
#define RBIPC_INTERNAL_H

#include "rbipc.h"
#include "rbipc_attr.h"
#include "rbipc_predicate.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/**
 * @struct rbipc_ring
 * @brief Concrete runtime handle for an attached ring buffer instance.
 */
struct rbipc_ring {
    int fd;                       /**< Shared memory file descriptor */
    char name[256];               /**< POSIX shared memory name */
    rbipc_shm_header_t *hdr;      /**< Pointer to control header in shared memory */
    rbipc_slot_t *slots;          /**< Pointer to slot descriptors array in shared memory */
    void *ctrl_map;               /**< Base virtual pointer to metadata map */
    size_t ctrl_map_size;         /**< Size of metadata map (page aligned) */
    void *data_map;               /**< Contiguous 2x double-mapped virtual buffer */
    size_t data_size;             /**< Size of single circular buffer */
    bool is_creator;              /**< True if created by this process */
    pid_t cached_pid;             /**< Cached PID of current process (avoids SYS_getpid syscalls) */
};

/**
 * @brief Safe, bounded string copy guaranteeing null-termination without insecure API warnings.
 *
 * @param dest Destination buffer.
 * @param dest_size Size of destination buffer in bytes.
 * @param src Null-terminated source string.
 */
RBIPC_INLINE void rbipc_str_copy(char *dest, size_t dest_size, const char *src) {
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
 * @brief Predicate: Check if an allocated ring handle satisfies operational invariants.
 *
 * @param ring Ring handle to validate.
 * @return true if non-null and all mapped pointers and descriptors are initialized.
 */
RBIPC_INLINE RBIPC_PURE bool rbipc_ring_is_valid(const rbipc_ring_t *ring) {
    if (RBIPC_UNLIKELY(!ring)) return false;
    if (RBIPC_UNLIKELY(!ring->hdr || !ring->slots || !ring->ctrl_map || !ring->data_map)) return false;
    return rbipc_header_is_valid(ring->hdr);
}

#endif /* RBIPC_INTERNAL_H */
