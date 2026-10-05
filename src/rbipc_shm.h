/**
 * @file rbipc_shm.h
 * @brief Shared memory file descriptor lifecycle, sizing, and sealing
 */

#ifndef RBIPC_SHM_H
#define RBIPC_SHM_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint32_t capacity;
    uint32_t aligned_slot_size;
    size_t header_and_slots_size;
    size_t data_offset;
    size_t data_size;
    size_t total_shm_size;
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
int rbipc_shm_calc_layout(uint32_t capacity, uint32_t slot_size, size_t page_size, rbipc_layout_t *layout);

/**
 * @brief Create and truncate a new shared memory file descriptor (POSIX shm or memfd).
 *
 * @param name Optional POSIX shm name (starts with '/'). If NULL, anonymous memfd is created.
 * @param total_size Size in bytes to truncate file to.
 * @param[out] out_fd Receives opened file descriptor.
 * @return RBIPC_OK on success, negative error code otherwise.
 */
int rbipc_shm_create(const char *name, size_t total_size, int *out_fd);

/**
 * @brief Open an existing POSIX shared memory file.
 *
 * @param name POSIX shm name.
 * @param[out] out_fd Receives opened file descriptor.
 * @return RBIPC_OK on success, negative error code otherwise.
 */
int rbipc_shm_open(const char *name, int *out_fd);

/**
 * @brief Apply Linux file sealing (F_SEAL_SHRINK | F_SEAL_GROW) to prevent resizing.
 *
 * @param fd Shared memory file descriptor.
 * @return RBIPC_OK on success, negative error code if sealing fails (ignored on older kernels).
 */
int rbipc_shm_seal(int fd);

/**
 * @brief Unlink a POSIX shared memory object.
 *
 * @param name POSIX shm name.
 * @return RBIPC_OK on success, negative error code otherwise.
 */
int rbipc_shm_unlink(const char *name);

/**
 * @brief Safely close a file descriptor.
 *
 * @param fd File descriptor to close.
 */
void rbipc_shm_close(int fd);

#endif /* RBIPC_SHM_H */
