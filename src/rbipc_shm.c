/**
 * @file rbipc_shm.c
 * @brief Implementation of shared memory descriptor management
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "rbipc.h"
#include "rbipc_shm.h"
#include "rbipc_math.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#ifndef F_LINUX_SPECIFIC_BASE
#define F_LINUX_SPECIFIC_BASE 1024
#endif
#ifndef F_ADD_SEALS
#define F_ADD_SEALS (F_LINUX_SPECIFIC_BASE + 9)
#define F_GET_SEALS (F_LINUX_SPECIFIC_BASE + 10)
#define F_SEAL_SEAL   0x0001
#define F_SEAL_SHRINK 0x0002
#define F_SEAL_GROW   0x0004
#endif

int rbipc_shm_calc_layout(uint32_t capacity, uint32_t slot_size, size_t page_size, rbipc_layout_t *layout) {
    if (!layout || capacity < 2 || slot_size == 0 || page_size == 0) {
        return RBIPC_ERR_INVAL;
    }

    /* Check capacity is a power of two */
    if ((capacity & (capacity - 1)) != 0) {
        return RBIPC_ERR_INVAL;
    }

    if (slot_size > UINT32_MAX - RBIPC_CACHE_LINE) {
        return RBIPC_ERR_OVERFLOW;
    }
    size_t aligned_slot_size = rbipc_align_up(slot_size, RBIPC_CACHE_LINE);
    if (aligned_slot_size == 0 || aligned_slot_size > UINT32_MAX) {
        return RBIPC_ERR_OVERFLOW;
    }

    /* Check for overflow in capacity * aligned_slot_size */
    if (aligned_slot_size > SIZE_MAX / capacity) {
        return RBIPC_ERR_OVERFLOW;
    }
    size_t raw_data_size = (size_t)capacity * aligned_slot_size;

    size_t data_size = rbipc_align_up(raw_data_size, page_size);
    if (data_size < raw_data_size) {
        return RBIPC_ERR_OVERFLOW;
    }

    /* Check slots table size overflow */
    if (sizeof(rbipc_slot_t) > (SIZE_MAX - sizeof(rbipc_shm_header_t)) / capacity) {
        return RBIPC_ERR_OVERFLOW;
    }
    size_t header_and_slots = sizeof(rbipc_shm_header_t) + ((size_t)capacity * sizeof(rbipc_slot_t));

    size_t data_offset = rbipc_align_up(header_and_slots, page_size);
    if (data_offset < header_and_slots) {
        return RBIPC_ERR_OVERFLOW;
    }

    if (data_size > SIZE_MAX - data_offset) {
        return RBIPC_ERR_OVERFLOW;
    }
    size_t total_shm_size = data_offset + data_size;

    layout->capacity = capacity;
    layout->aligned_slot_size = (uint32_t)aligned_slot_size;
    layout->header_and_slots_size = header_and_slots;
    layout->data_offset = data_offset;
    layout->data_size = data_size;
    layout->total_shm_size = total_shm_size;

    return RBIPC_OK;
}

int rbipc_shm_create(const char *name, size_t total_size, int *out_fd) {
    if (!out_fd || total_size == 0) {
        return RBIPC_ERR_INVAL;
    }

    int fd = -1;
    if (name) {
        if (name[0] != '/' || strlen(name) > 255) {
            return RBIPC_ERR_INVAL;
        }
        /* Remove any preexisting shm object with the same name */
        shm_unlink(name);
        fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0660);
    } else {
        fd = memfd_create("rbipc_anon", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    }

    if (fd < 0) {
        return RBIPC_ERR_SYS;
    }

    if (ftruncate(fd, (off_t)total_size) != 0) {
        int err = errno;
        close(fd);
        if (name) shm_unlink(name);
        errno = err;
        return RBIPC_ERR_SYS;
    }

    *out_fd = fd;
    return RBIPC_OK;
}

int rbipc_shm_open(const char *name, int *out_fd) {
    if (!name || !out_fd || name[0] != '/' || strlen(name) > 255) {
        return RBIPC_ERR_INVAL;
    }

    int fd = shm_open(name, O_RDWR, 0660);
    if (fd < 0) {
        return RBIPC_ERR_SYS;
    }

    *out_fd = fd;
    return RBIPC_OK;
}

int rbipc_shm_seal(int fd) {
    if (fd < 0) {
        return RBIPC_ERR_INVAL;
    }
#ifdef F_ADD_SEALS
    if (fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW) < 0) {
        /* Not fatal on all filesystems / kernels, but return RBIPC_OK or error */
        return RBIPC_OK;
    }
#endif
    return RBIPC_OK;
}

int rbipc_shm_unlink(const char *name) {
    if (!name || name[0] != '/') {
        return RBIPC_ERR_INVAL;
    }
    if (shm_unlink(name) != 0) {
        return RBIPC_ERR_SYS;
    }
    return RBIPC_OK;
}

void rbipc_shm_close(int fd) {
    if (fd >= 0) {
        close(fd);
    }
}
