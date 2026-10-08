/**
 * @file rbipc_vmem.h
 * @brief Virtual memory management and double-mapped circular ring mirror interfaces.
 * @details This header defines the primitives for mapping and unmapping control
 *          structures and constructing the continuous virtual memory ring mirror.
 *          By mapping the shared memory data region into two contiguous virtual address
 *          ranges using @c MAP_FIXED, read and write operations that cross the circular
 *          buffer boundary appear completely contiguous in virtual memory, thereby
 *          eliminating buffer wrap-around splits and payload copying.
 */

#ifndef RBIPC_VMEM_H
#define RBIPC_VMEM_H

#include "rbipc.h"
#include "rbipc_attr.h"

#include <stddef.h>

/**
 * @brief Maps the shared memory control header and slot metadata table into virtual memory.
 * @details Establishes a shared, read-write mapping (@c PROT_READ | @c PROT_WRITE, @c MAP_SHARED)
 *          spanning the control region from offset zero of the shared memory descriptor.
 *
 * @param[in]  fd       Shared memory file descriptor. Must be a valid, open descriptor.
 * @param[in]  size     Size in bytes of the control region to map. Must be non-zero and
 *                      aligned to the host virtual page boundary.
 * @param[out] out_map  Pointer to a memory address where the base address of the mapped
 *                      control region will be stored upon success. Must not be @c NULL.
 *
 * @return Status code indicating the result of the memory mapping operation.
 * @retval RBIPC_OK        Control region successfully mapped.
 * @retval RBIPC_ERR_INVAL Invalid descriptor, zero size, or null output pointer.
 * @retval RBIPC_ERR_SYS   Kernel @c mmap system call failed; inspect @c errno for root cause.
 */
RBIPC_NODISCARD
int rbipc_vmem_map_ctrl(int fd, size_t size, void ** RBIPC_RESTRICT out_map);

/**
 * @brief Unmaps the control header and slot metadata virtual memory region.
 * @details Releases the virtual address space reservation previously established
 *          by rbipc_vmem_map_ctrl(). Safe against @c NULL pointers and zero sizes.
 *
 * @param[in] map  Base virtual address of the mapped control region.
 * @param[in] size Size in bytes of the mapped control region.
 */
RBIPC_LEAF
void rbipc_vmem_unmap_ctrl(void *map, size_t size);

/**
 * @brief Constructs a double-mapped contiguous virtual address mirror for the ring data buffer.
 * @details To achieve zero-copy contiguous access across circular buffer wrap-around boundaries,
 *          this function:
 *          1. Reserves an uncommitted virtual address range of size \(2 \times \text{data\_size}\)
 *             using an anonymous private mapping (@c PROT_NONE, @c MAP_PRIVATE | @c MAP_ANONYMOUS).
 *          2. Overwrites the first half \([ \text{anon}, \text{anon} + \text{data\_size} )\)
 *             with a shared mapping of the underlying shared memory object at @p offset.
 *          3. Overwrites the second half \([ \text{anon} + \text{data\_size}, \text{anon} + 2 \times \text{data\_size} )\)
 *             with another shared mapping pointing to the exact same shared memory @p offset.
 *
 *          As a result, access spanning past the end of the first half seamlessly wraps
 *          to the beginning of the underlying physical storage via CPU MMU translation.
 *
 * @param[in]  fd           Shared memory file descriptor.
 * @param[in]  offset       Byte offset within the shared memory object where data begins.
 *                          Must be page-aligned.
 * @param[in]  data_size    Size in bytes of a single circular data buffer. Must be non-zero
 *                          and aligned to the host virtual page size.
 * @param[out] out_data_map Pointer to a memory address where the contiguous double-mapped
 *                          base address will be stored upon success.
 *
 * @return Status code indicating the outcome of the mirror mapping procedure.
 * @retval RBIPC_OK         Contiguous 2x mirror successfully mapped.
 * @retval RBIPC_ERR_INVAL  Invalid file descriptor, zero size, or null output pointer.
 * @retval RBIPC_ERR_NOMEM  Failed to reserve 2x contiguous virtual address space.
 * @retval RBIPC_ERR_SYS    Failed to map physical backing store to one of the mirror halves.
 */
RBIPC_NODISCARD
int rbipc_vmem_map_double(int fd, size_t offset, size_t data_size, void ** RBIPC_RESTRICT out_data_map);

/**
 * @brief Unmaps the double-mapped virtual buffer mirror.
 * @details Releases the entire \(2 \times \text{data\_size}\) contiguous virtual memory
 *          region previously established by rbipc_vmem_map_double().
 *
 * @param[in] data_map  Contiguous double-mapped base pointer.
 * @param[in] data_size Size in bytes of a single buffer (releases \(2 \times \text{data\_size}\)).
 */
RBIPC_LEAF
void rbipc_vmem_unmap_double(void *data_map, size_t data_size);

#endif /* RBIPC_VMEM_H */

