/**
 * @file rbipc_shm.h
 * @brief Shared memory file descriptor lifecycle, sizing, and sealing
 */

#ifndef RBIPC_SHM_H
#define RBIPC_SHM_H

#include "rbipc.h"
#include "rbipc_attr.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/**
 * @struct rbipc_layout_t
 * @brief Precomputed geometric memory layout offsets and sizes.
 */
typedef struct {
    uint32_t capacity;              /**< Validated slot capacity (power of two) */
    uint32_t aligned_slot_size;     /**< Cache-line aligned payload size */
    size_t header_and_slots_size;   /**< Exact byte size of header + slot table */
    size_t data_offset;             /**< Page-aligned offset to ring data buffer */
    size_t data_size;               /**< Page-aligned byte size of single circular buffer */
    size_t total_shm_size;          /**< Total shared memory file allocation size */
} rbipc_layout_t;

/**
 * @brief Calculate layout offsets and sizes with strict overflow validation.
 *
 * @param capacity Number of slots (must be power of two >= 2).
 * @param slot_size Requested payload size per slot.
 * @param page_size System page size.
 * @param[out] layout Computed layout metrics.
 * @return RBIPC_OK on success, negative error code on overflow or invalid arguments.
 */
RBIPC_NODISCARD RBIPC_LEAF
int rbipc_shm_calc_layout(uint32_t capacity, uint32_t slot_size, size_t page_size, rbipc_layout_t * RBIPC_RESTRICT layout);

/**
 * @brief Create and truncate a new shared memory file descriptor (POSIX shm or memfd).
 *
 * @param name Optional POSIX shm name (starts with '/'). If NULL, anonymous memfd is created.
 * @param total_size Size in bytes to truncate file to.
 * @param[out] out_fd Receives opened file descriptor.
 * @return RBIPC_OK on success, negative error code otherwise.
 */
RBIPC_NODISCARD
int rbipc_shm_create(const char * RBIPC_RESTRICT name, size_t total_size, int * RBIPC_RESTRICT out_fd);

/**
 * @brief Open an existing POSIX shared memory file.
 *
 * @param name POSIX shm name.
 * @param[out] out_fd Receives opened file descriptor.
 * @return RBIPC_OK on success, negative error code otherwise.
 */
RBIPC_NODISCARD
int rbipc_shm_open(const char * RBIPC_RESTRICT name, int * RBIPC_RESTRICT out_fd);

/**
 * @brief Apply Linux file sealing (F_SEAL_SHRINK | F_SEAL_GROW) to prevent resizing.
 *
 * @param fd Shared memory file descriptor.
 * @return RBIPC_OK on success, negative error code if sealing fails (ignored on older kernels).
 */
RBIPC_LEAF
int rbipc_shm_seal(int fd);

/**
 * @brief Unlink a POSIX shared memory object.
 *
 * @param name POSIX shm name.
 * @return RBIPC_OK on success, negative error code otherwise.
 */
RBIPC_LEAF
int rbipc_shm_unlink(const char *name);

/**
 * @brief Safely close a file descriptor.
 *
 * @param fd File descriptor to close.
 */
RBIPC_LEAF
void rbipc_shm_close(int fd);

#endif /* RBIPC_SHM_H */
