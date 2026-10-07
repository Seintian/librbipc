/**
 * @file rbipc_vmem.c
 * @brief Implementation of virtual memory mapping and double-mapped buffer
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "rbipc.h"
#include "rbipc_vmem.h"
#include "rbipc_attr.h"
#include "rbipc_predicate.h"

#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>

int rbipc_vmem_map_ctrl(int fd, size_t size, void **out_map) {
    if (RBIPC_UNLIKELY(!rbipc_is_valid_fd(fd) || size == 0 || !out_map)) {
        return RBIPC_ERR_INVAL;
    }

    void *ctrl = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ctrl == MAP_FAILED) {
        return RBIPC_ERR_SYS;
    }

    *out_map = ctrl;
    return RBIPC_OK;
}

void rbipc_vmem_unmap_ctrl(void *map, size_t size) {
    if (map && size > 0) {
        munmap(map, size);
    }
}

/**
 * @brief Atomic helper: Reserve 2x contiguous uncommitted virtual address space.
 */
RBIPC_INLINE int rbipc_vmem_reserve_address_space(size_t total_size, void **out_anon) {
    void *anon = mmap(NULL, total_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (anon == MAP_FAILED) {
        return RBIPC_ERR_NOMEM;
    }
    *out_anon = anon;
    return RBIPC_OK;
}

/**
 * @brief Atomic helper: Overwrite both halves of reserved address space with MAP_FIXED shared mappings.
 */
RBIPC_INLINE int rbipc_vmem_bind_mirror_halves(int fd, size_t offset, size_t data_size, void *anon) {
    /* Map first circular buffer half */
    void *m1 = mmap(anon, data_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, (off_t)offset);
    if (m1 == MAP_FAILED) {
        return RBIPC_ERR_SYS;
    }

    /* Map second circular buffer half to the exact same shared memory offset */
    void *m2 = mmap((char *)anon + data_size, data_size, PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_FIXED, fd, (off_t)offset);
    if (m2 == MAP_FAILED) {
        return RBIPC_ERR_SYS;
    }

    return RBIPC_OK;
}

int rbipc_vmem_map_double(int fd, size_t offset, size_t data_size, void **out_data_map) {
    if (RBIPC_UNLIKELY(!rbipc_is_valid_fd(fd) || data_size == 0 || !out_data_map)) {
        return RBIPC_ERR_INVAL;
    }

    void *anon = NULL;
    int rc = rbipc_vmem_reserve_address_space(2 * data_size, &anon);
    if (RBIPC_UNLIKELY(rc != RBIPC_OK)) {
        return rc;
    }

    rc = rbipc_vmem_bind_mirror_halves(fd, offset, data_size, anon);
    if (RBIPC_UNLIKELY(rc != RBIPC_OK)) {
        munmap(anon, 2 * data_size);
        return rc;
    }

    *out_data_map = anon;
    return RBIPC_OK;
}

void rbipc_vmem_unmap_double(void *data_map, size_t data_size) {
    if (data_map && data_size > 0) {
        munmap(data_map, 2 * data_size);
    }
}
