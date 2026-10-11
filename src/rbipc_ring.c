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
                                          uint32_t cap, uint32_t roles) {
    atomic_init(&hdr->magic, 0);
    hdr->version = htole32(RBIPC_VERSION);
    hdr->_reserved0 = 0;
    hdr->total_shm_size = htole64((uint64_t)layout->total_shm_size);
    hdr->data_offset = htole64((uint64_t)layout->data_offset);
    hdr->data_size = htole64((uint64_t)layout->data_size);
    hdr->capacity = htole32(cap);
    hdr->capacity_mask = htole32(cap - 1);
    hdr->slot_size = htole32(layout->aligned_slot_size);
    hdr->header_crc = htole32(rbipc_header_calc_crc(hdr));

    atomic_init(&hdr->write_ticket, 0);
    atomic_init(&hdr->read_ticket, 0);
    atomic_init(&hdr->futex_seq, 0);
    atomic_init(&hdr->futex_waiters, 0);
    atomic_init(&hdr->write_futex_seq, 0);
    atomic_init(&hdr->write_waiters, 0);
    atomic_init(&hdr->active_producers, (roles & RBIPC_ROLE_PRODUCER) ? 1 : 0);
    atomic_init(&hdr->active_consumers, (roles & RBIPC_ROLE_CONSUMER) ? 1 : 0);
    atomic_init(&hdr->shutdown_flag, 0);
    atomic_init(&hdr->eventfd_waiters, 0);
    atomic_init(&hdr->lease_timeout_ns, 0);
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
                                      uint32_t roles,
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
    ring->roles = roles;
    ring->is_creator = is_creator;
    ring->cached_pid = getpid();
    ring->event_fd = -1;
    ring->owns_eventfd = false;

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
 * @param[in] data_map  Data mapping address (unmapped if non-null).
 * @param[in] data_size Single buffer data size.
 */
static void rbipc_ring_abort_create(int fd, const char *name, void *ctrl_map,
                                    size_t ctrl_size, void *data_map, size_t data_size) {
    if (data_map && data_size > 0) {
        rbipc_vmem_unmap_data(data_map, data_size);
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
int rbipc_create_ex(const char * RBIPC_RESTRICT name, size_t capacity, uint32_t slot_size, uint32_t flags, mode_t mode, rbipc_ring_t ** RBIPC_RESTRICT out_ring) {
    if (RBIPC_UNLIKELY(!out_ring)) {
        return RBIPC_ERR_INVAL;
    }
    uint32_t known_flags = RBIPC_CREATE_OVERWRITE | RBIPC_ROLE_PRODUCER | RBIPC_ROLE_CONSUMER | RBIPC_ROLE_MONITOR;
    if (RBIPC_UNLIKELY((flags & ~known_flags) != 0)) {
        return RBIPC_ERR_INVAL;
    }
    uint32_t roles = flags & (RBIPC_ROLE_PRODUCER | RBIPC_ROLE_CONSUMER | RBIPC_ROLE_MONITOR);
    if (roles == 0) {
        roles = RBIPC_ROLE_PRODUCER;
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
    uint32_t shm_flags = flags & RBIPC_CREATE_OVERWRITE;
    rc = rbipc_shm_create_ex(name, layout.total_shm_size, shm_flags, mode, &fd);
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
    rbipc_ring_init_header_fields(hdr, &layout, cap, roles);

    rbipc_slot_t *slots = (rbipc_slot_t *)((char *)ctrl_map + sizeof(rbipc_shm_header_t));
    rbipc_slot_init_table(slots, cap);

    void *data_map = NULL;
    rc = rbipc_vmem_map_data(fd, layout.data_offset, layout.data_size, &data_map);
    if (RBIPC_UNLIKELY(rc != RBIPC_OK)) {
        rbipc_ring_abort_create(fd, name, ctrl_map, layout.data_offset, NULL, 0);
        return rc;
    }

    rbipc_ring_t *ring = rbipc_ring_alloc_handle(fd, name, hdr, slots, ctrl_map,
                                                 layout.data_offset, data_map,
                                                 layout.data_size, roles, true);
    if (RBIPC_UNLIKELY(!ring)) {
        rbipc_ring_abort_create(fd, name, ctrl_map, layout.data_offset, data_map, layout.data_size);
        return RBIPC_ERR_NOMEM;
    }

    /* Approach 1 & 3: Publish magic sentinel as the final step with release semantics and canonical endianness */
    atomic_store_explicit(&hdr->magic, htole64(RBIPC_MAGIC), memory_order_release);

    *out_ring = ring;
    return RBIPC_OK;
}

/**
 * @brief Create a named shared-memory IPC ring buffer with default options.
 */
int rbipc_create(const char * RBIPC_RESTRICT name, size_t capacity, uint32_t slot_size, rbipc_ring_t ** RBIPC_RESTRICT out_ring) {
    return rbipc_create_ex(name, capacity, slot_size, RBIPC_CREATE_EXCL, 0, out_ring);
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
 * @details Validates shared memory geometry, maps control metadata, maps the shared data
 *          buffer, increments consumer participant counters, and returns a runtime handle.
 */
int rbipc_attach_fd_ex(int fd, uint32_t role_flags, rbipc_ring_t ** RBIPC_RESTRICT out_ring) {
    if (RBIPC_UNLIKELY(!rbipc_is_valid_fd(fd) || !out_ring)) {
        return RBIPC_ERR_INVAL;
    }
    uint32_t known_roles = RBIPC_ROLE_PRODUCER | RBIPC_ROLE_CONSUMER | RBIPC_ROLE_MONITOR;
    if (RBIPC_UNLIKELY(role_flags == 0 || (role_flags & ~known_roles) != 0)) {
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
    rc = rbipc_vmem_map_data(fd, layout.data_offset, layout.data_size, &data_map);
    if (RBIPC_UNLIKELY(rc != RBIPC_OK)) {
        rbipc_vmem_unmap_ctrl(ctrl_map, layout.data_offset);
        return rc;
    }

    rbipc_ring_t *ring = rbipc_ring_alloc_handle(fd, NULL, hdr, slots, ctrl_map,
                                                 layout.data_offset, data_map,
                                                 layout.data_size, role_flags, false);
    if (RBIPC_UNLIKELY(!ring)) {
        rbipc_vmem_unmap_data(data_map, layout.data_size);
        rbipc_vmem_unmap_ctrl(ctrl_map, layout.data_offset);
        return RBIPC_ERR_NOMEM;
    }

    if (role_flags & RBIPC_ROLE_PRODUCER) {
        atomic_fetch_add_explicit(&hdr->active_producers, 1, memory_order_relaxed);
    }
    if (role_flags & RBIPC_ROLE_CONSUMER) {
        atomic_fetch_add_explicit(&hdr->active_consumers, 1, memory_order_relaxed);
    }

    *out_ring = ring;
    return RBIPC_OK;
}

/**
 * @brief Attach to an existing shared-memory ring buffer via open file descriptor as consumer.
 */
int rbipc_attach_fd(int fd, rbipc_ring_t ** RBIPC_RESTRICT out_ring) {
    return rbipc_attach_fd_ex(fd, RBIPC_ROLE_CONSUMER, out_ring);
}

/**
 * @brief Attach to an existing named shared-memory ring buffer with role flags.
 */
int rbipc_attach_ex(const char * RBIPC_RESTRICT name, uint32_t role_flags, rbipc_ring_t ** RBIPC_RESTRICT out_ring) {
    if (RBIPC_UNLIKELY(!name || !out_ring)) {
        return RBIPC_ERR_INVAL;
    }

    int fd = -1;
    int rc = rbipc_shm_open(name, &fd);
    if (RBIPC_UNLIKELY(rc != RBIPC_OK)) {
        return rc;
    }

    rc = rbipc_attach_fd_ex(fd, role_flags, out_ring);
    if (RBIPC_UNLIKELY(rc != RBIPC_OK)) {
        rbipc_shm_close(fd);
        return rc;
    }

    rbipc_str_copy((*out_ring)->name, sizeof((*out_ring)->name), name);
    return RBIPC_OK;
}

/**
 * @brief Attach to an existing named shared-memory ring buffer as consumer.
 */
int rbipc_attach(const char * RBIPC_RESTRICT name, rbipc_ring_t ** RBIPC_RESTRICT out_ring) {
    return rbipc_attach_ex(name, RBIPC_ROLE_CONSUMER, out_ring);
}

/**
 * @brief Releases virtual memory mappings associated with a ring handle.
 * @details Safely unmaps the shared data mapping and the control metadata mapping.
 */
RBIPC_LEAF
void rbipc_ring_unmap_regions(rbipc_ring_t *ring) {
    if (!ring) return;

    if (ring->data_map && ring->data_size > 0) {
        rbipc_vmem_unmap_data(ring->data_map, ring->data_size);
        ring->data_map = NULL;
    }

    if (ring->ctrl_map && ring->ctrl_map_size > 0) {
        rbipc_vmem_unmap_ctrl(ring->ctrl_map, ring->ctrl_map_size);
        ring->ctrl_map = NULL;
    }
}

/**
 * @brief Decrements active participant counters according to declared process roles.
 * @details Atomically decrements active_producers if RBIPC_ROLE_PRODUCER is configured,
 *          and active_consumers if RBIPC_ROLE_CONSUMER is configured.
 *
 * @param[in,out] ring Pointer to ring handle.
 */
static void rbipc_ring_deregister(rbipc_ring_t *ring) {
    if (!ring || !ring->hdr) return;
    if (ring->roles & RBIPC_ROLE_PRODUCER) {
        atomic_fetch_sub_explicit(&ring->hdr->active_producers, 1, memory_order_relaxed);
    }
    if (ring->roles & RBIPC_ROLE_CONSUMER) {
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

    if (ring->event_fd >= 0) {
        if (ring->hdr) {
            atomic_fetch_sub_explicit(&ring->hdr->eventfd_waiters, 1, memory_order_release);
        }
        if (ring->owns_eventfd) {
            close(ring->event_fd);
        }
        ring->event_fd = -1;
        ring->owns_eventfd = false;
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
    rbipc_ring_signal_eventfd(ring);
    return RBIPC_OK;
}

/**
 * @brief Enable asynchronous eventfd signaling on the ring buffer.
 */
int rbipc_enable_eventfd(rbipc_ring_t * RBIPC_RESTRICT ring, int * RBIPC_RESTRICT out_fd) {
    if (RBIPC_UNLIKELY(!ring || !out_fd)) {
        return RBIPC_ERR_INVAL;
    }
    if (ring->event_fd >= 0) {
        *out_fd = ring->event_fd;
        return RBIPC_OK;
    }
    int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (RBIPC_UNLIKELY(fd < 0)) {
        return RBIPC_ERR_SYS;
    }
    ring->event_fd = fd;
    ring->owns_eventfd = true;
    if (ring->hdr) {
        atomic_fetch_add_explicit(&ring->hdr->eventfd_waiters, 1, memory_order_release);
    }
    *out_fd = fd;
    return RBIPC_OK;
}

/**
 * @brief Bind an external eventfd file descriptor to the ring buffer.
 */
int rbipc_set_eventfd(rbipc_ring_t * RBIPC_RESTRICT ring, int event_fd) {
    if (RBIPC_UNLIKELY(!ring || event_fd < 0)) {
        return RBIPC_ERR_INVAL;
    }
    if (ring->event_fd >= 0 && ring->owns_eventfd) {
        if (ring->hdr) {
            atomic_fetch_sub_explicit(&ring->hdr->eventfd_waiters, 1, memory_order_release);
        }
        close(ring->event_fd);
    }
    ring->event_fd = event_fd;
    ring->owns_eventfd = false;
    if (ring->hdr) {
        atomic_fetch_add_explicit(&ring->hdr->eventfd_waiters, 1, memory_order_release);
    }
    return RBIPC_OK;
}

/**
 * @brief Retrieve currently bound eventfd file descriptor from ring buffer.
 */
int rbipc_get_eventfd(const rbipc_ring_t * RBIPC_RESTRICT ring, int * RBIPC_RESTRICT out_fd) {
    if (RBIPC_UNLIKELY(!ring || !out_fd)) {
        return RBIPC_ERR_INVAL;
    }
    if (ring->event_fd < 0) {
        return RBIPC_ERR_NOTSUP;
    }
    *out_fd = ring->event_fd;
    return RBIPC_OK;
}

/**
 * @brief Drain accumulated eventfd counter.
 */
int rbipc_eventfd_drain(const rbipc_ring_t * RBIPC_RESTRICT ring) {
    if (RBIPC_UNLIKELY(!ring)) {
        return RBIPC_ERR_INVAL;
    }
    if (ring->event_fd < 0) {
        return RBIPC_ERR_NOTSUP;
    }
    uint64_t val = 0;
    ssize_t n = read(ring->event_fd, &val, sizeof(val));
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return RBIPC_OK;
        }
        return RBIPC_ERR_SYS;
    }
    return (n == sizeof(val)) ? RBIPC_OK : RBIPC_ERR_SYS;
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
    out_stats->eventfd_waiters = atomic_load_explicit(&hdr->eventfd_waiters, memory_order_relaxed);
    out_stats->is_shutdown = (atomic_load_explicit(&hdr->shutdown_flag, memory_order_relaxed) != 0);

    return RBIPC_OK;
}

RBIPC_LEAF
/**
 * @brief Set wait synchronization policy (zero-spin vs adaptive).
 */
int rbipc_set_wait_policy(rbipc_ring_t *ring, uint32_t policy) {
    if (RBIPC_UNLIKELY(!ring)) {
        return RBIPC_ERR_INVAL;
    }
    if (RBIPC_UNLIKELY(policy != RBIPC_WAIT_ZERO_SPIN && policy != RBIPC_WAIT_ADAPTIVE)) {
        return RBIPC_ERR_INVAL;
    }
    ring->wait_policy = policy;
    return RBIPC_OK;
}

RBIPC_LEAF
/**
 * @brief Query current wait synchronization policy.
 */
uint32_t rbipc_get_wait_policy(const rbipc_ring_t *ring) {
    if (RBIPC_UNLIKELY(!ring)) {
        return RBIPC_WAIT_ZERO_SPIN;
    }
    return ring->wait_policy;
}

RBIPC_LEAF
/**
 * @brief Configure consumer slot holding lease duration in nanoseconds.
 */
int rbipc_set_lease_timeout(rbipc_ring_t *ring, uint64_t lease_timeout_ns) {
    if (RBIPC_UNLIKELY(!ring || !ring->hdr)) {
        return RBIPC_ERR_INVAL;
    }
    atomic_store_explicit(&ring->hdr->lease_timeout_ns, lease_timeout_ns, memory_order_release);
    return RBIPC_OK;
}

RBIPC_LEAF
/**
 * @brief Query configured consumer slot holding lease duration in nanoseconds.
 */
uint64_t rbipc_get_lease_timeout(const rbipc_ring_t *ring) {
    if (RBIPC_UNLIKELY(!ring || !ring->hdr)) {
        return 0;
    }
    return atomic_load_explicit(&ring->hdr->lease_timeout_ns, memory_order_acquire);
}

RBIPC_LEAF
/**
 * @brief Proactively audit ring buffer and revoke expired consumer leases.
 */
int rbipc_revoke_expired_leases(rbipc_ring_t *ring) {
    if (RBIPC_UNLIKELY(!ring || !ring->hdr)) {
        return RBIPC_ERR_INVAL;
    }
    rbipc_shm_header_t *hdr = ring->hdr;
    uint64_t lease = atomic_load_explicit(&hdr->lease_timeout_ns, memory_order_relaxed);
    if (lease == 0) {
        return 0;
    }

    uint64_t now = rbipc_clock_monotonic_ns();
    rbipc_slot_t *slots = (rbipc_slot_t *)RBIPC_ASSUME_ALIGNED(ring->slots, RBIPC_CACHE_LINE);
    uint32_t cap = hdr->capacity;
    int revoked_count = 0;

    for (uint32_t i = 0; i < cap; ++i) {
        rbipc_slot_t *slot = &slots[i];
        uint32_t cpid = atomic_load_explicit(&slot->consumer_pid, memory_order_relaxed);
        if (cpid > 0) {
            uint64_t acquired_ts = atomic_load_explicit(&slot->acquire_timestamp_ns, memory_order_relaxed);
            if (acquired_ts > 0 && now > acquired_ts && (now - acquired_ts) >= lease) {
                uint64_t seq = atomic_load_explicit(&slot->sequence, memory_order_acquire);
                uint64_t target_ticket = (seq > 0 ? (seq - 1) : 0) + (uint64_t)cap;
                if (rbipc_slot_recover_expired_lease(slot, target_ticket, lease, now)) {
                    revoked_count++;
                }
            }
        }
    }

    if (revoked_count > 0) {
        atomic_thread_fence(memory_order_seq_cst);
        rbipc_sync_wake_all(&hdr->write_futex_seq, &hdr->write_waiters);
    }
    return revoked_count;
}
