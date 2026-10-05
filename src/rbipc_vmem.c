/**
 * @file rbipc_vmem.c
 * @brief Implementation of virtual memory mapping and double-mapped buffer
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "rbipc.h"
#include "rbipc_vmem.h"

#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>

int rbipc_vmem_map_ctrl(int fd, size_t size, void **out_map) {
    if (fd < 0 || size == 0 || !out_map) {
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

int rbipc_vmem_map_double(int fd, size_t offset, size_t data_size, void **out_data_map) {
    if (fd < 0 || data_size == 0 || !out_data_map) {
        return RBIPC_ERR_INVAL;
    }

    /* 1. Reserve 2 * data_size contiguous virtual address space */
    void *anon = mmap(NULL, 2 * data_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (anon == MAP_FAILED) {
        return RBIPC_ERR_NOMEM;
    }

    /* 2. Map first half directly to the shared memory data region */
    void *m1 = mmap(anon, data_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, (off_t)offset);
    if (m1 == MAP_FAILED) {
        munmap(anon, 2 * data_size);
        return RBIPC_ERR_SYS;
    }

    /* 3. Map second half to the EXACT SAME shared memory data region */
    void *m2 = mmap((char *)anon + data_size, data_size, PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_FIXED, fd, (off_t)offset);
    if (m2 == MAP_FAILED) {
        munmap(anon, 2 * data_size);
        return RBIPC_ERR_SYS;
    }

    *out_data_map = anon;
    return RBIPC_OK;
}

void rbipc_vmem_unmap_double(void *data_map, size_t data_size) {
    if (data_map && data_size > 0) {
        munmap(data_map, 2 * data_size);
    }
}
