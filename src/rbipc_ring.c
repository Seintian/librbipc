/**
 * @file rbipc_ring.c
 * @brief Shared-memory lifecycle management, handle allocation, and diagnostics.
 * @details Implements ring initialization (@c rbipc_create), multi-process attachment
 *          (@c rbipc_attach, @c rbipc_attach_fd), cooperative shutdown signaling
 *          (@c rbipc_signal_shutdown), resource teardown (@c rbipc_detach), and
 *          unlinking (@c rbipc_destroy).
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "rbipc.h"
#include "rbipc_internal.h"
#include "rbipc_ring.h"
#include "rbipc_math.h"
#include "rbipc_shm.h"
#include "rbipc_vmem.h"
#include "rbipc_slot.h"
#include "rbipc_sync.h"
#include "rbipc_attr.h"
#include "rbipc_predicate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
/* ============================================================================
 * Atomic Lifecycle Initialization and Teardown Subroutines
 * ============================================================================ */


/**
 * @brief Validates ring creation parameters and resolves the host page boundary.
 * @details Ensures capacity and slot size are non-zero, determines system virtual page size
 *          via @c sysconf(_SC_PAGESIZE), and rounds up capacity to the next power of two.
 *
 * @param[in]  capacity      Requested minimum ring capacity (number of slots).
 * @param[in]  slot_size     Maximum payload size per slot in bytes.
 * @param[out] out_page_size Pointer to receive the detected host page size.
 * @param[out] out_cap       Pointer to receive the validated power-of-two capacity.
 *
 * @return Status code indicating the result of parameter validation.
 * @retval RBIPC_OK           Parameters are valid.
 * @retval RBIPC_ERR_INVAL    Zero capacity or slot size.
 * @retval RBIPC_ERR_OVERFLOW Capacity exceeds 32-bit power-of-two ceiling.
 * @retval RBIPC_ERR_SYS      Failed to query host page size.
 */
static int rbipc_ring_validate_create_params(size_t capacity, uint32_t slot_size,
                                               size_t * RBIPC_RESTRICT out_page_size,
                                               uint32_t * RBIPC_RESTRICT out_cap) {
    if (capacity == 0 || !rbipc_is_valid_slot_size(slot_size)) {
        return RBIPC_ERR_INVAL;
    }

    long page_size_raw = sysconf(_SC_PAGESIZE);
    if (page_size_raw <= 0) return RBIPC_ERR_SYS;
    *out_page_size = (size_t)page_size_raw;

    uint32_t cap = rbipc_round_up_pow2_32((uint32_t)capacity);
    if (cap == 0 || cap > RBIPC_MAX_CAPACITY) {
        return RBIPC_ERR_OVERFLOW;
    }
    if (cap < RBIPC_MIN_CAPACITY) cap = RBIPC_MIN_CAPACITY;
    *out_cap = cap;

    return RBIPC_OK;
}

/**
 * @brief Initializes structural header control words, geometry attributes, and atomic counters.
 * @details Populates magic number, ABI version, capacity masks, aligned slot sizes,
 *          and initializes all atomic tickets and futex state words using @c atomic_init.
 *
 * @param[out] hdr    Pointer to the mapped shared memory control header.
 * @param[in]  layout Precomputed shared memory layout geometry.
 * @param[in]  cap    Normalized power-of-two ring capacity.
 */
static void rbipc_ring_init_header_fields(rbipc_shm_header_t * RBIPC_RESTRICT hdr,
                                          const rbipc_layout_t * RBIPC_RESTRICT layout,
                                          uint32_t cap) {
    hdr->magic = RBIPC_MAGIC;
    hdr->version = RBIPC_VERSION;
    hdr->total_shm_size = (uint64_t)layout->total_shm_size;
    hdr->data_offset = (uint64_t)layout->data_offset;
    hdr->data_size = (uint64_t)layout->data_size;
    hdr->capacity = cap;
    hdr->capacity_mask = cap - 1;
    hdr->slot_size = layout->aligned_slot_size;

    atomic_init(&hdr->write_ticket, 0);
    atomic_init(&hdr->read_ticket, 0);
    atomic_init(&hdr->futex_seq, 0);
    atomic_init(&hdr->futex_waiters, 0);
    atomic_init(&hdr->write_futex_seq, 0);
    atomic_init(&hdr->write_waiters, 0);
    atomic_init(&hdr->active_producers, 1);
    atomic_init(&hdr->active_consumers, 0);
    atomic_init(&hdr->shutdown_flag, 0);
}

/**
 * @brief Allocates heap memory and initializes a runtime ring handle descriptor.
 * @details Populates all runtime references, records creator status, and caches process PID.
 */
rbipc_ring_t *rbipc_ring_alloc_handle(int fd, const char * RBIPC_RESTRICT name,
                                      rbipc_shm_header_t *hdr,
                                      rbipc_slot_t *slots,
                                      void *ctrl_map, size_t ctrl_map_size,
                                      void *data_map, size_t data_size,
                                      bool is_creator) {
    rbipc_ring_t *ring = (rbipc_ring_t *)calloc(1, sizeof(rbipc_ring_t));
    if (RBIPC_UNLIKELY(!ring)) {
        return NULL;
    }

    ring->fd = fd;
    if (name) {
        rbipc_str_copy(ring->name, sizeof(ring->name), name);
    }
    ring->hdr = hdr;
    ring->slots = slots;
    ring->ctrl_map = ctrl_map;
    ring->ctrl_map_size = ctrl_map_size;
    ring->data_map = data_map;
    ring->data_size = data_size;
    ring->is_creator = is_creator;
    ring->cached_pid = getpid();

    return ring;
}

/**
 * @brief Aborts creation and rolls back all allocated mappings, descriptors, and named files.
 * @details Cleans up partially constructed resources during creation failure.
 *
 * @param[in] fd        Shared memory file descriptor.
 * @param[in] name      Shared memory object name (unlinked if non-null).
 * @param[in] ctrl_map  Mapped control region address (unmapped if non-null).
 * @param[in] ctrl_size Mapped control region byte size.
 * @param[in] data_map  Double-mapped data mirror address (unmapped if non-null).
 * @param[in] data_size Single buffer data size.
 */
static void rbipc_ring_abort_create(int fd, const char *name, void *ctrl_map,
                                    size_t ctrl_size, void *data_map, size_t data_size) {
    if (data_map && data_size > 0) {
        rbipc_vmem_unmap_double(data_map, data_size);
    }
    if (ctrl_map && ctrl_size > 0) {
        rbipc_vmem_unmap_ctrl(ctrl_map, ctrl_size);
    }
    if (fd >= 0) {
        rbipc_shm_close(fd);
    }
    if (name) {
        rbipc_shm_unlink(name);
    }
}

/**
 * @brief Creates a new shared memory IPC ring buffer and returns its runtime handle.
 * @details Implements full initialization sequence: parameter validation, layout calculation,
 *          POSIX shared memory / memfd allocation, file sealing, virtual memory mappings,
 *          and slot initialization.
 */
int rbipc_create(const char * RBIPC_RESTRICT name, size_t capacity, uint32_t slot_size, rbipc_ring_t ** RBIPC_RESTRICT out_ring) {
    if (RBIPC_UNLIKELY(!out_ring)) {
        return RBIPC_ERR_INVAL;
    }

    size_t page_size = 0;
    uint32_t cap = 0;
    int rc = rbipc_ring_validate_create_params(capacity, slot_size, &page_size, &cap);
    if (RBIPC_UNLIKELY(rc != RBIPC_OK)) {
        return rc;
    }

    rbipc_layout_t layout;
    rc = rbipc_shm_calc_layout(cap, slot_size, page_size, &layout);
    if (RBIPC_UNLIKELY(rc != RBIPC_OK)) {
        return rc;
    }

    int fd = -1;
    rc = rbipc_shm_create(name, layout.total_shm_size, &fd);
    if (RBIPC_UNLIKELY(rc != RBIPC_OK)) {
        return rc;
    }

    rbipc_shm_seal(fd);

    void *ctrl_map = NULL;
    rc = rbipc_vmem_map_ctrl(fd, layout.data_offset, &ctrl_map);
    if (RBIPC_UNLIKELY(rc != RBIPC_OK)) {
        rbipc_ring_abort_create(fd, name, NULL, 0, NULL, 0);
        return rc;
    }

    rbipc_shm_header_t *hdr = (rbipc_shm_header_t *)ctrl_map;
    rbipc_ring_init_header_fields(hdr, &layout, cap);

    rbipc_slot_t *slots = (rbipc_slot_t *)((char *)ctrl_map + sizeof(rbipc_shm_header_t));
    rbipc_slot_init_table(slots, cap);

    void *data_map = NULL;
    rc = rbipc_vmem_map_double(fd, layout.data_offset, layout.data_size, &data_map);
    if (RBIPC_UNLIKELY(rc != RBIPC_OK)) {
        rbipc_ring_abort_create(fd, name, ctrl_map, layout.data_offset, NULL, 0);
        return rc;
    }

    rbipc_ring_t *ring = rbipc_ring_alloc_handle(fd, name, hdr, slots, ctrl_map,
                                                 layout.data_offset, data_map,
                                                 layout.data_size, true);
    if (RBIPC_UNLIKELY(!ring)) {
        rbipc_ring_abort_create(fd, name, ctrl_map, layout.data_offset, data_map, layout.data_size);
        return RBIPC_ERR_NOMEM;
    }

    *out_ring = ring;
    return RBIPC_OK;
}

/**
 * @brief Inspects and validates the header of an existing shared memory object.
 * @details Maps the initial page of the object, verifies magic, version, and layout consistency,
 *          and populates @p out_layout before unmapping the probe page.
 */
int rbipc_ring_probe_and_validate_header(int fd, size_t page_size, rbipc_layout_t * RBIPC_RESTRICT out_layout) {
    void *probe_map = NULL;
    int rc = rbipc_vmem_map_ctrl(fd, page_size, &probe_map);
    if (RBIPC_UNLIKELY(rc != RBIPC_OK)) {
        return rc;
    }

    const rbipc_shm_header_t *hdr_check = (const rbipc_shm_header_t *)probe_map;
    if (RBIPC_UNLIKELY(!rbipc_header_is_valid(hdr_check))) {
        rbipc_vmem_unmap_ctrl(probe_map, page_size);
        return RBIPC_ERR_INVAL;
    }

    size_t data_offset = (size_t)hdr_check->data_offset;
    size_t data_size = (size_t)hdr_check->data_size;
    uint32_t cap = hdr_check->capacity;
    uint32_t slot_size = hdr_check->slot_size;

    rbipc_vmem_unmap_ctrl(probe_map, page_size);

    rc = rbipc_shm_calc_layout(cap, slot_size, page_size, out_layout);
    if (RBIPC_UNLIKELY(rc != RBIPC_OK || out_layout->data_offset != data_offset || out_layout->data_size != data_size)) {
        return RBIPC_ERR_INVAL;
    }

    return RBIPC_OK;
}

/**
 * @brief Attaches to an existing ring buffer via an open file descriptor.
 * @details Validates shared memory geometry, maps control metadata, binds the double-mapped
 *          mirror, increments consumer participant counters, and returns a runtime handle.
 */
int rbipc_attach_fd(int fd, rbipc_ring_t ** RBIPC_RESTRICT out_ring) {
    if (RBIPC_UNLIKELY(!rbipc_is_valid_fd(fd) || !out_ring)) {
        return RBIPC_ERR_INVAL;
    }

    long page_size_raw = sysconf(_SC_PAGESIZE);
    if (page_size_raw <= 0) return RBIPC_ERR_SYS;
    size_t page_size = (size_t)page_size_raw;

    rbipc_layout_t layout;
    int rc = rbipc_ring_probe_and_validate_header(fd, page_size, &layout);
    if (RBIPC_UNLIKELY(rc != RBIPC_OK)) {
        return rc;
    }

    void *ctrl_map = NULL;
    rc = rbipc_vmem_map_ctrl(fd, layout.data_offset, &ctrl_map);
    if (RBIPC_UNLIKELY(rc != RBIPC_OK)) {
        return rc;
    }

    rbipc_shm_header_t *hdr = (rbipc_shm_header_t *)ctrl_map;
    rbipc_slot_t *slots = (rbipc_slot_t *)((char *)ctrl_map + sizeof(rbipc_shm_header_t));

    void *data_map = NULL;
    rc = rbipc_vmem_map_double(fd, layout.data_offset, layout.data_size, &data_map);
    if (RBIPC_UNLIKELY(rc != RBIPC_OK)) {
        rbipc_vmem_unmap_ctrl(ctrl_map, layout.data_offset);
        return rc;
    }

    rbipc_ring_t *ring = rbipc_ring_alloc_handle(fd, NULL, hdr, slots, ctrl_map,
                                                 layout.data_offset, data_map,
                                                 layout.data_size, false);
    if (RBIPC_UNLIKELY(!ring)) {
        rbipc_vmem_unmap_double(data_map, layout.data_size);
        rbipc_vmem_unmap_ctrl(ctrl_map, layout.data_offset);
        return RBIPC_ERR_NOMEM;
    }

    atomic_fetch_add_explicit(&hdr->active_consumers, 1, memory_order_relaxed);

    *out_ring = ring;
    return RBIPC_OK;
}

/**
 * @brief Attaches to an existing ring buffer by its POSIX shared memory name.
 * @details Opens the named shared memory object and delegates to rbipc_attach_fd().
 */
int rbipc_attach(const char * RBIPC_RESTRICT name, rbipc_ring_t ** RBIPC_RESTRICT out_ring) {
    if (RBIPC_UNLIKELY(!name || !out_ring)) {
        return RBIPC_ERR_INVAL;
    }

    int fd = -1;
    int rc = rbipc_shm_open(name, &fd);
    if (RBIPC_UNLIKELY(rc != RBIPC_OK)) {
        return rc;
    }

    rc = rbipc_attach_fd(fd, out_ring);
    if (RBIPC_UNLIKELY(rc != RBIPC_OK)) {
        rbipc_shm_close(fd);
        return rc;
    }

    rbipc_str_copy((*out_ring)->name, sizeof((*out_ring)->name), name);
    return RBIPC_OK;
}

/**
 * @brief Releases virtual memory mappings associated with a ring handle.
 * @details Safely unmaps the double-mapped data mirror and the control metadata mapping.
 */
RBIPC_LEAF
void rbipc_ring_unmap_regions(rbipc_ring_t *ring) {
    if (!ring) return;

    if (ring->data_map && ring->data_size > 0) {
        rbipc_vmem_unmap_double(ring->data_map, ring->data_size);
        ring->data_map = NULL;
    }

    if (ring->ctrl_map && ring->ctrl_map_size > 0) {
        rbipc_vmem_unmap_ctrl(ring->ctrl_map, ring->ctrl_map_size);
        ring->ctrl_map = NULL;
    }
}

/**
 * @brief Decrements active participant counter according to process role.
 * @details Decrements active_producers if creator, otherwise active_consumers.
 *
 * @param[in] ring Pointer to ring handle.
 */
static void rbipc_ring_deregister(rbipc_ring_t *ring) {
    if (!ring || !ring->hdr) return;
    if (ring->is_creator) {
        atomic_fetch_sub_explicit(&ring->hdr->active_producers, 1, memory_order_relaxed);
    } else {
        atomic_fetch_sub_explicit(&ring->hdr->active_consumers, 1, memory_order_relaxed);
    }
}

/**
 * @brief Detaches a process from a ring buffer and frees local runtime resources.
 * @details Updates participant counts, unmaps memory regions, closes the descriptor,
 *          and frees the heap descriptor.
 */
int rbipc_detach(rbipc_ring_t *ring) {
    if (RBIPC_UNLIKELY(!ring)) {
        return RBIPC_ERR_INVAL;
    }

    rbipc_ring_deregister(ring);
    rbipc_ring_unmap_regions(ring);

    if (ring->fd >= 0) {
        rbipc_shm_close(ring->fd);
        ring->fd = -1;
    }

    free(ring);
    return RBIPC_OK;
}

/**
 * @brief Unlinks and destroys a named shared memory object from the system.
 * @details Calls @c shm_unlink to remove the named shared memory file from @c /dev/shm.
 */
int rbipc_destroy(const char *name) {
    return rbipc_shm_unlink(name);
}

/**
 * @brief Broadcasts a cooperative shutdown notification to all participants.
 * @details Sets shutdown_flag with @c memory_order_release and wakes all suspended
 *          readers and writers via futex broadcasts.
 */
int rbipc_signal_shutdown(rbipc_ring_t *ring) {
    if (RBIPC_UNLIKELY(!ring || !ring->hdr)) {
        return RBIPC_ERR_INVAL;
    }

    atomic_store_explicit(&ring->hdr->shutdown_flag, 1, memory_order_release);
    rbipc_sync_wake_all(&ring->hdr->futex_seq, &ring->hdr->futex_waiters);
    rbipc_sync_wake_all(&ring->hdr->write_futex_seq, &ring->hdr->write_waiters);
    return RBIPC_OK;
}

/**
 * @brief Retrieves the underlying shared memory file descriptor.
 */
RBIPC_LEAF
int rbipc_get_fd(const rbipc_ring_t *ring) {
    return ring ? ring->fd : -1;
}

/**
 * @brief Samples real-time operational metrics and diagnostic counters.
 * @details Reads shared memory dimensions, tickets, and active participant counters.
 */
RBIPC_LEAF
int rbipc_get_stats(const rbipc_ring_t * RBIPC_RESTRICT ring, rbipc_stats_t * RBIPC_RESTRICT out_stats) {
    if (RBIPC_UNLIKELY(!ring || !ring->hdr || !out_stats)) {
        return RBIPC_ERR_INVAL;
    }

    rbipc_shm_header_t *hdr = ring->hdr;
    out_stats->total_shm_size = hdr->total_shm_size;
    out_stats->data_size = hdr->data_size;
    out_stats->capacity = hdr->capacity;
    out_stats->slot_size = hdr->slot_size;
    out_stats->write_ticket = atomic_load_explicit(&hdr->write_ticket, memory_order_relaxed);
    out_stats->read_ticket = atomic_load_explicit(&hdr->read_ticket, memory_order_relaxed);
    out_stats->active_producers = atomic_load_explicit(&hdr->active_producers, memory_order_relaxed);
    out_stats->active_consumers = atomic_load_explicit(&hdr->active_consumers, memory_order_relaxed);
    out_stats->futex_waiters = atomic_load_explicit(&hdr->futex_waiters, memory_order_relaxed) +
                               atomic_load_explicit(&hdr->write_waiters, memory_order_relaxed);
    out_stats->is_shutdown = (atomic_load_explicit(&hdr->shutdown_flag, memory_order_relaxed) != 0);

    return RBIPC_OK;
}
