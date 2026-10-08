/**
 * @file rbipc_ring.h
 * @brief Internal ring buffer lifecycle prototypes and helper interfaces.
 * @details This header declares procedures for initializing runtime handles,
 *          probing shared memory layouts during client attachment, and releasing
 *          associated virtual memory regions.
 */

#ifndef RBIPC_RING_H
#define RBIPC_RING_H

#include "rbipc.h"
#include "rbipc_internal.h"
#include "rbipc_shm.h"

#include <stdbool.h>
#include <stddef.h>

/**
 * @brief Allocates and initializes a heap-allocated ring buffer runtime handle.
 * @details Allocates a @c struct @c rbipc_ring instance via @c calloc, records all memory
 *          mappings and offsets, populates descriptor metadata, and caches the current process PID.
 *
 * @param[in] fd            Shared memory file descriptor.
 * @param[in] name          Optional POSIX shared memory object name (may be @c NULL for anonymous descriptors).
 * @param[in] hdr           Pointer to the mapped control header in virtual memory.
 * @param[in] slots         Pointer to the array of mapped slot descriptors in virtual memory.
 * @param[in] ctrl_map      Base virtual address of the mapped control region.
 * @param[in] ctrl_map_size Size in bytes of the mapped control region.
 * @param[in] data_map      Base virtual address of the contiguous double-mapped data mirror.
 * @param[in] data_size     Size in bytes of a single circular data buffer.
 * @param[in] is_creator    @c true if this process initialized the shared memory region; @c false otherwise.
 *
 * @return Pointer to the allocated and initialized ring handle, or @c NULL if heap allocation fails.
 */
RBIPC_NODISCARD
rbipc_ring_t *rbipc_ring_alloc_handle(int fd, const char * RBIPC_RESTRICT name,
                                      rbipc_shm_header_t *hdr,
                                      rbipc_slot_t *slots,
                                      void *ctrl_map, size_t ctrl_map_size,
                                      void *data_map, size_t data_size,
                                      bool is_creator);

/**
 * @brief Probes, inspects, and validates a shared memory object's header and layout.
 * @details Temporarily maps the first page of the shared memory object at @p fd to inspect
 *          the header magic, ABI protocol version, and structural dimensions. Recomputes
 *          the theoretical memory layout and verifies that it strictly matches the header.
 *
 * @param[in]  fd         Shared memory file descriptor.
 * @param[in]  page_size  System virtual page size in bytes.
 * @param[out] out_layout Pointer to receive the verified memory geometry layout.
 *
 * @return Status code indicating the outcome of the header inspection.
 * @retval RBIPC_OK        Header is valid and structural layout is consistent.
 * @retval RBIPC_ERR_INVAL Invalid magic, version mismatch, or corrupted geometry fields.
 * @retval RBIPC_ERR_SYS   Failed to map the probe page.
 */
RBIPC_NODISCARD
int rbipc_ring_probe_and_validate_header(int fd, size_t page_size, rbipc_layout_t * RBIPC_RESTRICT out_layout);

/**
 * @brief Safely unmaps all virtual memory regions associated with a ring handle.
 * @details Unmaps the double-mapped data mirror and the control metadata mapping,
 *          nullifying their pointers in the ring structure.
 *
 * @param[in,out] ring Pointer to the ring handle whose mappings are to be released.
 */
RBIPC_LEAF
void rbipc_ring_unmap_regions(rbipc_ring_t *ring);

#endif /* RBIPC_RING_H */

