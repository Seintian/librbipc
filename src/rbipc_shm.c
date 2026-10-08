/**
 * @file rbipc_shm.c
 * @brief Implementation of shared memory descriptor management and layout geometry.
 *
 * @details Implements overflow-safe geometric partition sizing, file descriptor creation
 * via shm_open or memfd_create, kernel file sealing, and descriptor teardown.
 *
 * @author Christian Santarelli
 * @date 2026
 * @copyright Apache License 2.0
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "rbipc.h"
#include "rbipc_shm.h"
#include "rbipc_math.h"
#include "rbipc_attr.h"
#include "rbipc_predicate.h"

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

/**
 * @brief Compute cache-line aligned slot byte capacity with overflow checking.
 *
 * @param[in]  slot_size   Raw unaligned payload size.
 * @param[out] out_aligned Pointer receiving aligned slot size.
 *
 * @return @ref RBIPC_OK on success, @ref RBIPC_ERR_OVERFLOW if value overflows 32 bits.
 */
RBIPC_INLINE int rbipc_shm_calc_aligned_slot_size(uint32_t slot_size, size_t * RBIPC_RESTRICT out_aligned) {
    if (slot_size > UINT32_MAX - RBIPC_CACHE_LINE) {
        return RBIPC_ERR_OVERFLOW;
    }
    size_t aligned = rbipc_align_up(slot_size, RBIPC_CACHE_LINE);
    if (aligned == 0 || aligned > UINT32_MAX) {
        return RBIPC_ERR_OVERFLOW;
    }
    *out_aligned = aligned;
    return RBIPC_OK;
}

/**
 * @brief Compute page-aligned single circular data buffer size with multiplication overflow check.
 *
 * @param[in]  capacity          Slot count.
 * @param[in]  aligned_slot_size Aligned slot byte size.
 * @param[in]  page_size         System page size.
 * @param[out] out_data_size     Pointer receiving page-aligned data buffer byte size.
 *
 * @return @ref RBIPC_OK on success, @ref RBIPC_ERR_OVERFLOW on overflow.
 */
RBIPC_INLINE int rbipc_shm_calc_data_geometry(uint32_t capacity, size_t aligned_slot_size,
                                              size_t page_size, size_t * RBIPC_RESTRICT out_data_size) {
    if (aligned_slot_size > SIZE_MAX / capacity) {
        return RBIPC_ERR_OVERFLOW;
    }
    size_t raw_data_size = (size_t)capacity * aligned_slot_size;
    size_t data_size = rbipc_align_up(raw_data_size, page_size);
    if (data_size < raw_data_size) {
        return RBIPC_ERR_OVERFLOW;
    }
    *out_data_size = data_size;
    return RBIPC_OK;
}

/**
 * @brief Compute metadata structure size and page-aligned payload data offset.
 *
 * @param[in]  capacity          Slot count.
 * @param[in]  page_size         System page size.
 * @param[out] out_hdr_and_slots Pointer receiving exact header + slots byte size.
 * @param[out] out_data_offset   Pointer receiving page-aligned offset to ring data buffer.
 *
 * @return @ref RBIPC_OK on success, @ref RBIPC_ERR_OVERFLOW on overflow.
 */
RBIPC_INLINE int rbipc_shm_calc_metadata_geometry(uint32_t capacity, size_t page_size,
                                                  size_t * RBIPC_RESTRICT out_hdr_and_slots,
                                                  size_t * RBIPC_RESTRICT out_data_offset) {
    if (sizeof(rbipc_slot_t) > (SIZE_MAX - sizeof(rbipc_shm_header_t)) / capacity) {
        return RBIPC_ERR_OVERFLOW;
    }
    size_t header_and_slots = sizeof(rbipc_shm_header_t) + ((size_t)capacity * sizeof(rbipc_slot_t));
    size_t data_offset = rbipc_align_up(header_and_slots, page_size);
    if (data_offset < header_and_slots) {
        return RBIPC_ERR_OVERFLOW;
    }
    *out_hdr_and_slots = header_and_slots;
    *out_data_offset = data_offset;
    return RBIPC_OK;
}

RBIPC_LEAF
int rbipc_shm_calc_layout(uint32_t capacity, uint32_t slot_size, size_t page_size, rbipc_layout_t * RBIPC_RESTRICT layout) {
    if (RBIPC_UNLIKELY(!layout || !rbipc_is_power_of_two(capacity) || capacity < 2 || slot_size == 0 || page_size == 0)) {
        return RBIPC_ERR_INVAL;
    }

    size_t aligned_slot_size = 0;
    int rc = rbipc_shm_calc_aligned_slot_size(slot_size, &aligned_slot_size);
    if (RBIPC_UNLIKELY(rc != RBIPC_OK)) return rc;

    size_t data_size = 0;
    rc = rbipc_shm_calc_data_geometry(capacity, aligned_slot_size, page_size, &data_size);
    if (RBIPC_UNLIKELY(rc != RBIPC_OK)) return rc;

    size_t header_and_slots = 0;
    size_t data_offset = 0;
    rc = rbipc_shm_calc_metadata_geometry(capacity, page_size, &header_and_slots, &data_offset);
    if (RBIPC_UNLIKELY(rc != RBIPC_OK)) return rc;

    if (RBIPC_UNLIKELY(data_size > SIZE_MAX - data_offset)) {
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

/**
 * @brief Atomic helper: Create an anonymous memory file descriptor via Linux memfd_create.
 */
RBIPC_INLINE int rbipc_shm_create_anonymous(int * RBIPC_RESTRICT out_fd) {
    int fd = memfd_create("rbipc_anon", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) {
        return RBIPC_ERR_SYS;
    }
    *out_fd = fd;
    return RBIPC_OK;
}

/**
 * @brief Atomic helper: Create and exclusively open a POSIX shared memory object.
 */
RBIPC_INLINE int rbipc_shm_create_named(const char * RBIPC_RESTRICT name, int * RBIPC_RESTRICT out_fd) {
    if (RBIPC_UNLIKELY(!rbipc_is_valid_shm_name(name))) {
        return RBIPC_ERR_INVAL;
    }
    /* Clean up pre-existing stale file if any */
    shm_unlink(name);
    int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0660);
    if (fd < 0) {
        return RBIPC_ERR_SYS;
    }
    *out_fd = fd;
    return RBIPC_OK;
}

/**
 * @brief Atomic helper: Truncate file descriptor to desired byte capacity.
 */
RBIPC_INLINE int rbipc_shm_truncate_file(int fd, size_t total_size) {
    if (ftruncate(fd, (off_t)total_size) != 0) {
        return RBIPC_ERR_SYS;
    }
    return RBIPC_OK;
}

int rbipc_shm_create(const char * RBIPC_RESTRICT name, size_t total_size, int * RBIPC_RESTRICT out_fd) {
    if (RBIPC_UNLIKELY(!out_fd || total_size == 0)) {
        return RBIPC_ERR_INVAL;
    }

    int fd = -1;
    int rc = name ? rbipc_shm_create_named(name, &fd) : rbipc_shm_create_anonymous(&fd);
    if (RBIPC_UNLIKELY(rc != RBIPC_OK)) {
        return rc;
    }

    rc = rbipc_shm_truncate_file(fd, total_size);
    if (RBIPC_UNLIKELY(rc != RBIPC_OK)) {
        int err = errno;
        close(fd);
        if (name) shm_unlink(name);
        errno = err;
        return rc;
    }

    *out_fd = fd;
    return RBIPC_OK;
}

int rbipc_shm_open(const char * RBIPC_RESTRICT name, int * RBIPC_RESTRICT out_fd) {
    if (RBIPC_UNLIKELY(!out_fd || !rbipc_is_valid_shm_name(name))) {
        return RBIPC_ERR_INVAL;
    }

    int fd = shm_open(name, O_RDWR, 0660);
    if (fd < 0) {
        return RBIPC_ERR_SYS;
    }

    *out_fd = fd;
    return RBIPC_OK;
}

RBIPC_LEAF
int rbipc_shm_seal(int fd) {
    if (RBIPC_UNLIKELY(!rbipc_is_valid_fd(fd))) {
        return RBIPC_ERR_INVAL;
    }
#ifdef F_ADD_SEALS
    if (fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW) < 0) {
        /* Not fatal on all filesystems / kernels */
        return RBIPC_OK;
    }
#endif
    return RBIPC_OK;
}

RBIPC_LEAF
int rbipc_shm_unlink(const char *name) {
    if (RBIPC_UNLIKELY(!rbipc_is_valid_shm_name(name))) {
        return RBIPC_ERR_INVAL;
    }
    if (shm_unlink(name) != 0) {
        return RBIPC_ERR_SYS;
    }
    return RBIPC_OK;
}

RBIPC_LEAF
void rbipc_shm_close(int fd) {
    if (rbipc_is_valid_fd(fd)) {
        close(fd);
    }
}
