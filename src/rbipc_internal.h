/**
 * @file rbipc_internal.h
 * @brief Internal definitions and ring structure for librbipc
 */

#ifndef RBIPC_INTERNAL_H
#define RBIPC_INTERNAL_H

#include "rbipc.h"
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

struct rbipc_ring {
    int fd;                       /**< Shared memory file descriptor */
    char name[256];               /**< POSIX shared memory name */
    rbipc_shm_header_t *hdr;      /**< Pointer to control header */
    rbipc_slot_t *slots;          /**< Pointer to slot descriptors array */
    void *ctrl_map;               /**< Base virtual pointer to metadata map */
    size_t ctrl_map_size;         /**< Size of metadata map (page aligned) */
    void *data_map;               /**< Contiguous 2x double-mapped virtual buffer */
    size_t data_size;             /**< Size of single buffer */
    bool is_creator;              /**< True if created by this process */
    pid_t cached_pid;             /**< Cached PID of current process (avoids SYS_getpid syscalls) */
};

#endif /* RBIPC_INTERNAL_H */
