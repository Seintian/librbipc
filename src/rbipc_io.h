/**
 * @file rbipc_io.h
 * @brief Internal I/O pipeline helpers, address calculations, and prefetching
 */

#ifndef RBIPC_IO_H
#define RBIPC_IO_H

#include "rbipc.h"
#include "rbipc_internal.h"
#include "rbipc_attr.h"

/**
 * @brief Atomic helper: Calculate zero-copy virtual address for a slot ticket.
 */
RBIPC_INLINE RBIPC_PURE void *rbipc_io_calc_slot_ptr(const rbipc_ring_t *ring, uint32_t ticket) {
    uint32_t idx = ticket & ring->hdr->capacity_mask;
    return (char *)ring->data_map + ((size_t)idx * ring->hdr->slot_size);
}

/**
 * @brief Atomic helper: Prefetch future slot descriptor and data payload into CPU cache for write.
 */
RBIPC_INLINE void rbipc_io_prefetch_write_next(const rbipc_ring_t *ring, uint32_t next_ticket) {
    uint32_t idx = next_ticket & ring->hdr->capacity_mask;
    __builtin_prefetch(&ring->slots[idx], 1, 3);
    __builtin_prefetch((char *)ring->data_map + ((size_t)idx * ring->hdr->slot_size), 1, 1);
}

/**
 * @brief Atomic helper: Prefetch future slot descriptor and data payload into CPU cache for read.
 */
RBIPC_INLINE void rbipc_io_prefetch_read_next(const rbipc_ring_t *ring, uint32_t next_ticket) {
    uint32_t idx = next_ticket & ring->hdr->capacity_mask;
    __builtin_prefetch(&ring->slots[idx], 0, 3);
    __builtin_prefetch((const char *)ring->data_map + ((size_t)idx * ring->hdr->slot_size), 0, 1);
}

#endif /* RBIPC_IO_H */
