/**
 * @file rbipc_ring.h
 * @brief Internal ring buffer lifecycle prototypes and helper interfaces
 */

#ifndef RBIPC_RING_H
#define RBIPC_RING_H

#include "rbipc.h"
#include "rbipc_internal.h"
#include "rbipc_shm.h"

#include <stdbool.h>
#include <stddef.h>

/**
 * @brief Allocate and populate a ring buffer runtime handle.
 */
rbipc_ring_t *rbipc_ring_alloc_handle(int fd, const char *name,
                                      rbipc_shm_header_t *hdr,
                                      rbipc_slot_t *slots,
                                      void *ctrl_map, size_t ctrl_map_size,
                                      void *data_map, size_t data_size,
                                      bool is_creator);

/**
 * @brief Probe, inspect, and validate a shared memory object's header and layout.
 */
int rbipc_ring_probe_and_validate_header(int fd, size_t page_size, rbipc_layout_t *out_layout);

/**
 * @brief Safely unmap virtual memory mappings associated with a ring handle.
 */
void rbipc_ring_unmap_regions(rbipc_ring_t *ring);

#endif /* RBIPC_RING_H */
