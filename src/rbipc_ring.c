/**
 * @file rbipc_ring.c
 * @brief Lifecycle management, attachment, and statistics for librbipc
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "rbipc.h"
#include "rbipc_internal.h"
#include "rbipc_math.h"
#include "rbipc_shm.h"
#include "rbipc_vmem.h"
#include "rbipc_slot.h"
#include "rbipc_sync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#define DEFAULT_SPIN_THRESHOLD 50000

int rbipc_create(const char *name, size_t capacity, uint32_t slot_size, rbipc_ring_t **out_ring) {
    if (!out_ring || capacity == 0 || slot_size == 0) {
        return RBIPC_ERR_INVAL;
    }

    long page_size_raw = sysconf(_SC_PAGESIZE);
    if (page_size_raw <= 0) return RBIPC_ERR_SYS;
    size_t page_size = (size_t)page_size_raw;

    uint32_t cap = rbipc_round_up_pow2_32((uint32_t)capacity);
    if (cap == 0) {
        return RBIPC_ERR_OVERFLOW;
    }
    if (cap < 2) cap = 2;

    rbipc_layout_t layout;
    int rc = rbipc_shm_calc_layout(cap, slot_size, page_size, &layout);
    if (rc != RBIPC_OK) {
        return rc;
    }

    int fd = -1;
    rc = rbipc_shm_create(name, layout.total_shm_size, &fd);
    if (rc != RBIPC_OK) {
        return rc;
    }

    /* Apply file sealing */
    rbipc_shm_seal(fd);

    /* Map control region */
    void *ctrl_map = NULL;
    rc = rbipc_vmem_map_ctrl(fd, layout.data_offset, &ctrl_map);
    if (rc != RBIPC_OK) {
        rbipc_shm_close(fd);
        if (name) rbipc_shm_unlink(name);
        return rc;
    }

    /* Initialize control header */
    rbipc_shm_header_t *hdr = (rbipc_shm_header_t *)ctrl_map;
    memset(hdr, 0, sizeof(*hdr));
    hdr->magic = RBIPC_MAGIC;
    hdr->version = RBIPC_VERSION;
    hdr->header_size = sizeof(*hdr);
    hdr->total_shm_size = (uint64_t)layout.total_shm_size;
    hdr->data_offset = (uint64_t)layout.data_offset;
    hdr->data_size = (uint64_t)layout.data_size;
    hdr->capacity = cap;
    hdr->capacity_mask = cap - 1;
    hdr->slot_size = layout.aligned_slot_size;
    hdr->spin_threshold = DEFAULT_SPIN_THRESHOLD;

    atomic_init(&hdr->write_ticket, 0);
    atomic_init(&hdr->read_ticket, 0);
    atomic_init(&hdr->futex_seq, 0);
    atomic_init(&hdr->active_producers, 1);
    atomic_init(&hdr->active_consumers, 0);
    atomic_init(&hdr->shutdown_flag, 0);

    /* Initialize slots */
    rbipc_slot_t *slots = (rbipc_slot_t *)((char *)ctrl_map + sizeof(rbipc_shm_header_t));
    rbipc_slot_init_table(slots, cap);

    /* Double-map data region */
    void *data_map = NULL;
    rc = rbipc_vmem_map_double(fd, layout.data_offset, layout.data_size, &data_map);
    if (rc != RBIPC_OK) {
        rbipc_vmem_unmap_ctrl(ctrl_map, layout.data_offset);
        rbipc_shm_close(fd);
        if (name) rbipc_shm_unlink(name);
        return rc;
    }

    rbipc_ring_t *ring = (rbipc_ring_t *)calloc(1, sizeof(rbipc_ring_t));
    if (!ring) {
        rbipc_vmem_unmap_double(data_map, layout.data_size);
        rbipc_vmem_unmap_ctrl(ctrl_map, layout.data_offset);
        rbipc_shm_close(fd);
        if (name) rbipc_shm_unlink(name);
        return RBIPC_ERR_NOMEM;
    }

    ring->fd = fd;
    if (name) {
        strncpy(ring->name, name, sizeof(ring->name) - 1);
    }
    ring->hdr = hdr;
    ring->slots = slots;
    ring->ctrl_map = ctrl_map;
    ring->ctrl_map_size = layout.data_offset;
    ring->data_map = data_map;
    ring->data_size = layout.data_size;
    ring->is_creator = true;

    *out_ring = ring;
    return RBIPC_OK;
}

int rbipc_attach_fd(int fd, rbipc_ring_t **out_ring) {
    if (fd < 0 || !out_ring) {
        return RBIPC_ERR_INVAL;
    }

    long page_size_raw = sysconf(_SC_PAGESIZE);
    if (page_size_raw <= 0) return RBIPC_ERR_SYS;
    size_t page_size = (size_t)page_size_raw;

    /* Map minimal probe page to inspect and validate header */
    void *probe_map = NULL;
    int rc = rbipc_vmem_map_ctrl(fd, page_size, &probe_map);
    if (rc != RBIPC_OK) {
        return rc;
    }

    rbipc_shm_header_t *hdr_check = (rbipc_shm_header_t *)probe_map;
    if (hdr_check->magic != RBIPC_MAGIC || hdr_check->version != RBIPC_VERSION) {
        rbipc_vmem_unmap_ctrl(probe_map, page_size);
        return RBIPC_ERR_INVAL;
    }

    size_t data_offset = (size_t)hdr_check->data_offset;
    size_t data_size = (size_t)hdr_check->data_size;
    uint32_t cap = hdr_check->capacity;
    uint32_t slot_size = hdr_check->slot_size;

    rbipc_vmem_unmap_ctrl(probe_map, page_size);

    /* Verify layout parameters */
    rbipc_layout_t layout;
    rc = rbipc_shm_calc_layout(cap, slot_size, page_size, &layout);
    if (rc != RBIPC_OK || layout.data_offset != data_offset || layout.data_size != data_size) {
        return RBIPC_ERR_INVAL;
    }

    /* Map full control region */
    void *ctrl_map = NULL;
    rc = rbipc_vmem_map_ctrl(fd, data_offset, &ctrl_map);
    if (rc != RBIPC_OK) {
        return rc;
    }

    rbipc_shm_header_t *hdr = (rbipc_shm_header_t *)ctrl_map;
    rbipc_slot_t *slots = (rbipc_slot_t *)((char *)ctrl_map + sizeof(rbipc_shm_header_t));

    /* Map 2x double virtual ring */
    void *data_map = NULL;
    rc = rbipc_vmem_map_double(fd, data_offset, data_size, &data_map);
    if (rc != RBIPC_OK) {
        rbipc_vmem_unmap_ctrl(ctrl_map, data_offset);
        return rc;
    }

    rbipc_ring_t *ring = (rbipc_ring_t *)calloc(1, sizeof(rbipc_ring_t));
    if (!ring) {
        rbipc_vmem_unmap_double(data_map, data_size);
        rbipc_vmem_unmap_ctrl(ctrl_map, data_offset);
        return RBIPC_ERR_NOMEM;
    }

    ring->fd = fd;
    ring->hdr = hdr;
    ring->slots = slots;
    ring->ctrl_map = ctrl_map;
    ring->ctrl_map_size = data_offset;
    ring->data_map = data_map;
    ring->data_size = data_size;
    ring->is_creator = false;

    atomic_fetch_add_explicit(&hdr->active_consumers, 1, memory_order_relaxed);

    *out_ring = ring;
    return RBIPC_OK;
}

int rbipc_attach(const char *name, rbipc_ring_t **out_ring) {
    if (!name || !out_ring) {
        return RBIPC_ERR_INVAL;
    }

    int fd = -1;
    int rc = rbipc_shm_open(name, &fd);
    if (rc != RBIPC_OK) {
        return rc;
    }

    rc = rbipc_attach_fd(fd, out_ring);
    if (rc != RBIPC_OK) {
        rbipc_shm_close(fd);
        return rc;
    }

    strncpy((*out_ring)->name, name, sizeof((*out_ring)->name) - 1);
    return RBIPC_OK;
}

int rbipc_detach(rbipc_ring_t *ring) {
    if (!ring) {
        return RBIPC_ERR_INVAL;
    }

    if (ring->hdr) {
        if (ring->is_creator) {
            atomic_fetch_sub_explicit(&ring->hdr->active_producers, 1, memory_order_relaxed);
        } else {
            atomic_fetch_sub_explicit(&ring->hdr->active_consumers, 1, memory_order_relaxed);
        }
    }

    if (ring->data_map && ring->data_size > 0) {
        rbipc_vmem_unmap_double(ring->data_map, ring->data_size);
        ring->data_map = NULL;
    }

    if (ring->ctrl_map && ring->ctrl_map_size > 0) {
        rbipc_vmem_unmap_ctrl(ring->ctrl_map, ring->ctrl_map_size);
        ring->ctrl_map = NULL;
    }

    if (ring->fd >= 0) {
        rbipc_shm_close(ring->fd);
        ring->fd = -1;
    }

    free(ring);
    return RBIPC_OK;
}

int rbipc_destroy(const char *name) {
    return rbipc_shm_unlink(name);
}

int rbipc_signal_shutdown(rbipc_ring_t *ring) {
    if (!ring || !ring->hdr) {
        return RBIPC_ERR_INVAL;
    }

    atomic_store_explicit(&ring->hdr->shutdown_flag, 1, memory_order_release);
    rbipc_sync_wake_all(&ring->hdr->futex_seq);
    return RBIPC_OK;
}

int rbipc_get_fd(const rbipc_ring_t *ring) {
    return ring ? ring->fd : -1;
}

int rbipc_get_stats(const rbipc_ring_t *ring, rbipc_stats_t *out_stats) {
    if (!ring || !ring->hdr || !out_stats) {
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
    out_stats->is_shutdown = (atomic_load_explicit(&hdr->shutdown_flag, memory_order_relaxed) != 0);

    return RBIPC_OK;
}
