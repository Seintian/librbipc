/**
 * @file rbipc_vmem.h
 * @brief Virtual memory mapping abstractions and double-mapped circular ring mirror
 */

#ifndef RBIPC_VMEM_H
#define RBIPC_VMEM_H

#include "rbipc.h"
#include "rbipc_attr.h"

#include <stddef.h>

/**
 * @brief Map control header and slot metadata region.
 *
 * @param fd File descriptor to shared memory object.
 * @param size Size in bytes of control region (page aligned).
 * @param[out] out_map Pointer to mapped virtual address.
 * @return RBIPC_OK on success, negative error code otherwise.
 */
RBIPC_NODISCARD
int rbipc_vmem_map_ctrl(int fd, size_t size, void **out_map);

/**
 * @brief Unmap control header and slot metadata region.
 *
 * @param map Pointer to mapped virtual address.
 * @param size Size in bytes of control region.
 */
void rbipc_vmem_unmap_ctrl(void *map, size_t size);

/**
 * @brief Double-maps a single shared memory data region into contiguous 2x virtual address space.
 *
 * Reserves 2 * data_size contiguous virtual address space, then maps both halves
 * with MAP_FIXED to the exact same shared memory offset.
 *
 * @param fd Shared memory file descriptor.
 * @param offset Page-aligned offset in file to data region.
 * @param data_size Size of single circular buffer (page aligned).
 * @param[out] out_data_map Contiguous double-mapped base pointer.
 * @return RBIPC_OK on success, negative error code otherwise.
 */
RBIPC_NODISCARD
int rbipc_vmem_map_double(int fd, size_t offset, size_t data_size, void **out_data_map);

/**
 * @brief Unmap the double-mapped virtual buffer.
 *
 * @param data_map Contiguous double-mapped base pointer.
 * @param data_size Size of single circular buffer (unmaps 2 * data_size).
 */
void rbipc_vmem_unmap_double(void *data_map, size_t data_size);

#endif /* RBIPC_VMEM_H */
