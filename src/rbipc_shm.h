/**
 * @file rbipc_shm.h
 * @brief Shared memory file descriptor lifecycle, layout geometry, and kernel file sealing.
 *
 * @details Implements geometric calculations for shared memory offsets, file creation via
 * POSIX shm_open or Linux memfd_create, and sealing via fcntl F_ADD_SEALS to prevent SIGBUS panics.
 *
 * @author Christian Santarelli
 * @date 2026
 * @copyright Apache License 2.0
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
 * @brief Precomputed geometric memory layout offsets and partition boundaries.
 */
typedef struct {
    uint32_t capacity;              /**< Validated slot count (strictly a power of two). */
    uint32_t aligned_slot_size;     /**< Payload capacity per slot aligned to cache-line boundary (64 bytes). */
    size_t header_and_slots_size;   /**< Exact byte size of control header plus slot table. */
    size_t data_offset;             /**< Page-aligned byte offset within file where payload data begins. */
    size_t data_size;               /**< Page-aligned byte size of the single circular payload buffer. */
    size_t total_shm_size;          /**< Total shared memory file allocation size (metadata + data). */
} rbipc_layout_t;

/**
 * @brief Calculate shared memory geometric partitions with integer overflow checks.
 *
 * @details Validates capacity (power of two), aligns slot size to 64 bytes, computes
 * metadata size (header + slots), and aligns data offset and data size to system page size.
 *
 * @param[in]  capacity  Requested slot count.
 * @param[in]  slot_size Requested payload byte capacity per slot.
 * @param[in]  page_size Host virtual memory page size in bytes.
 * @param[out] layout    Precomputed layout geometry descriptor to populate.
 *
 * @return @ref RBIPC_OK on success.
 * @retval RBIPC_ERR_INVAL    if capacity is invalid, slot_size is zero, or page_size is invalid.
 * @retval RBIPC_ERR_OVERFLOW if arithmetic multiplication overflows size_t.
 */
RBIPC_NODISCARD RBIPC_LEAF
int rbipc_shm_calc_layout(uint32_t capacity, uint32_t slot_size, size_t page_size, rbipc_layout_t * RBIPC_RESTRICT layout);

/**
 * @brief Create and truncate a new shared memory file descriptor.
 *
 * @details If @p name is non-null, creates a named POSIX shared memory object via `shm_open(O_CREAT | O_EXCL)`.
 * If @p name is NULL, creates an anonymous Linux memory file descriptor via `memfd_create()`.
 * Truncates the opened file descriptor to @p total_size via `ftruncate()`.
 *
 * @param[in]  name       POSIX shared memory name, or NULL for anonymous memfd.
 * @param[in]  total_size Allocation byte size to truncate file to.
 * @param[out] out_fd     Location to store the opened file descriptor.
 *
 * @return @ref RBIPC_OK on success.
 * @retval RBIPC_ERR_INVAL if @p out_fd is NULL or @p total_size is zero.
 * @retval RBIPC_ERR_SYS   if shm_open, memfd_create, or ftruncate fails.
 */
RBIPC_NODISCARD
int rbipc_shm_create(const char * RBIPC_RESTRICT name, size_t total_size, int * RBIPC_RESTRICT out_fd);

/**
 * @brief Open an existing named POSIX shared memory file descriptor.
 *
 * @param[in]  name   POSIX shared memory name.
 * @param[out] out_fd Location to store the opened file descriptor.
 *
 * @return @ref RBIPC_OK on success.
 * @retval RBIPC_ERR_INVAL if @p name or @p out_fd is NULL.
 * @retval RBIPC_ERR_SYS   if shm_open fails.
 */
RBIPC_NODISCARD
int rbipc_shm_open(const char * RBIPC_RESTRICT name, int * RBIPC_RESTRICT out_fd);

/**
 * @brief Apply Linux file seals to prevent file resizing or truncation.
 *
 * @details Applies `F_SEAL_SHRINK | F_SEAL_GROW` via `fcntl(F_ADD_SEALS)` to protect
 * mapped memory against truncation SIGBUS signals.
 *
 * @param[in] fd Shared memory file descriptor.
 *
 * @return @ref RBIPC_OK on success or if sealing is unsupported by kernel.
 * @retval RBIPC_ERR_SYS if sealing fails unexpectedly.
 */
RBIPC_LEAF
int rbipc_shm_seal(int fd);

/**
 * @brief Unlink a named POSIX shared memory object.
 *
 * @param[in] name POSIX shared memory name.
 *
 * @return @ref RBIPC_OK on success.
 * @retval RBIPC_ERR_INVAL if @p name is NULL.
 * @retval RBIPC_ERR_SYS   if shm_unlink fails.
 */
RBIPC_LEAF
int rbipc_shm_unlink(const char *name);

/**
 * @brief Close an open file descriptor defensively.
 *
 * @param[in] fd File descriptor to close.
 */
RBIPC_LEAF
void rbipc_shm_close(int fd);

#endif /* RBIPC_SHM_H */
