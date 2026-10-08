/**
 * @file rbipc_io.h
 * @brief Internal I/O pipeline helpers, address calculations, and prefetching.
 * @details This header defines inline subroutines for fast slot address calculation,
 *          CPU cache prefetching for sequential write and read tickets, and memory alignment
 *          hints for the compiler optimizer.
 */

#ifndef RBIPC_IO_H
#define RBIPC_IO_H

#include "rbipc.h"
#include "rbipc_internal.h"
#include "rbipc_attr.h"

/**
 * @brief Calculates the zero-copy virtual address for a given ticket's slot data buffer.
 * @details Computes the slot index using bitwise AND with the capacity mask
 *          (\(\text{ticket} \ \& \ \text{capacity\_mask}\)) and offsets into the
 *          cacheline-aligned double-mapped memory base pointer.
 *
 * @param[in] ring   Pointer to the active ring runtime handle.
 * @param[in] ticket Monotonic ticket value identifying the target slot.
 *
 * @return Direct pointer in virtual memory to the beginning of the slot's data buffer.
 */
RBIPC_INLINE RBIPC_PURE void *rbipc_io_calc_slot_ptr(const rbipc_ring_t *ring, uint32_t ticket) {
    uint32_t idx = ticket & ring->hdr->capacity_mask;
    void *data_map = RBIPC_ASSUME_ALIGNED(ring->data_map, RBIPC_CACHE_LINE);
    return (char *)data_map + ((size_t)idx * ring->hdr->slot_size);
}

/**
 * @brief Prefetches future slot metadata and payload memory lines into CPU cache for writing.
 * @details Issues @c __builtin_prefetch for the slot descriptor with write intent (@c rw=1)
 *          and high temporal locality (@c locality=3), and for the payload buffer with
 *          moderate temporal locality (@c locality=1). This mitigates LLC misses on upcoming writes.
 *
 * @param[in] ring        Pointer to the ring handle.
 * @param[in] next_ticket The upcoming write ticket scheduled for subsequent reservation.
 */
RBIPC_INLINE void rbipc_io_prefetch_write_next(const rbipc_ring_t *ring, uint32_t next_ticket) {
    uint32_t idx = next_ticket & ring->hdr->capacity_mask;
    RBIPC_PREFETCH(&ring->slots[idx], 1, 3);
    void *data_map = RBIPC_ASSUME_ALIGNED(ring->data_map, RBIPC_CACHE_LINE);
    RBIPC_PREFETCH((char *)data_map + ((size_t)idx * ring->hdr->slot_size), 1, 1);
}

/**
 * @brief Prefetches future slot metadata and payload memory lines into CPU cache for reading.
 * @details Issues @c __builtin_prefetch for the slot descriptor with read intent (@c rw=0)
 *          and high temporal locality (@c locality=3), and for the payload buffer with
 *          moderate temporal locality (@c locality=1).
 *
 * @param[in] ring        Pointer to the ring handle.
 * @param[in] next_ticket The upcoming read ticket scheduled for subsequent acquisition.
 */
RBIPC_INLINE void rbipc_io_prefetch_read_next(const rbipc_ring_t *ring, uint32_t next_ticket) {
    uint32_t idx = next_ticket & ring->hdr->capacity_mask;
    RBIPC_PREFETCH(&ring->slots[idx], 0, 3);
    const void *data_map = RBIPC_ASSUME_ALIGNED(ring->data_map, RBIPC_CACHE_LINE);
    RBIPC_PREFETCH((const char *)data_map + ((size_t)idx * ring->hdr->slot_size), 0, 1);
}

#endif /* RBIPC_IO_H */

