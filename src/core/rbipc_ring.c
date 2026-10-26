/**
 * @file rbipc_ring.c
 * @brief Shared-memory lifecycle management, handle allocation, and
 * diagnostics.
 *
 * @details Implements ring initialization (@c rbipc_create, @c
 * rbipc_create_ex), multi-process attachment (@c rbipc_attach, @c
 * rbipc_attach_fd), cooperative shutdown signaling (@c rbipc_signal_shutdown),
 * resource teardown (@c rbipc_detach), and unlinking (@c rbipc_destroy).
 *
 * @author Seintian
 * @date 2026
 * @copyright Apache License 2.0
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "rbipc_ring.h"
#include "rbipc.h"
#include "rbipc_arch.h"
#include "rbipc_attr.h"
#include "rbipc_audit.h"
#include "rbipc_futex.h"
#include "rbipc_internal.h"
#include "rbipc_math.h"
#include "rbipc_predicate.h"
#include "rbipc_reactor.h"
#include "rbipc_shm.h"
#include "rbipc_slot.h"
#include "rbipc_sync.h"
#include "rbipc_vmem.h"

#include "rbipc_cxl.h"
#include "rbipc_mpk.h"
#include "rbipc_waitfree.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ============================================================================
 * Atomic Lifecycle Initialization and Teardown Subroutines
 * ============================================================================
 */

/**
 * @brief Validates ring creation parameters and resolves the host page
 * boundary.
 */
static int
rbipc_ring_validate_create_params(size_t capacity, uint32_t slot_size,
                                  size_t *RBIPC_RESTRICT out_page_size,
                                  uint32_t *RBIPC_RESTRICT out_cap) {
  if (capacity == 0 || !rbipc_is_valid_slot_size(slot_size)) {
    return RBIPC_ERR_INVAL;
  }
  if (capacity > (size_t)RBIPC_MAX_CAPACITY) {
    return RBIPC_ERR_OVERFLOW;
  }

  size_t page_size = rbipc_page_size();
  *out_page_size = page_size;

  uint32_t cap = rbipc_round_up_pow2_32((uint32_t)capacity);
  if (cap == 0 || cap > RBIPC_MAX_CAPACITY) {
    return RBIPC_ERR_OVERFLOW;
  }
  if (cap < RBIPC_MIN_CAPACITY)
    cap = RBIPC_MIN_CAPACITY;
  *out_cap = cap;

  return RBIPC_OK;
}

/**
 * @brief Initializes structural header control words, geometry attributes, and
 * atomic counters.
 */
static void
rbipc_ring_init_header_fields(rbipc_shm_header_t *RBIPC_RESTRICT hdr,
                              const rbipc_layout_t *RBIPC_RESTRICT layout,
                              uint32_t cap, uint32_t roles, uint32_t flags) {
  atomic_init(&hdr->magic, 0);
  hdr->version = htole32(RBIPC_VERSION);
  hdr->flags = htole32(flags & (RBIPC_FLAG_TRUSTED | RBIPC_FLAG_PACKED |
                                RBIPC_FLAG_HUGETLB | RBIPC_FLAG_WAIT_FREE |
                                RBIPC_FLAG_ISOLATED | RBIPC_FLAG_CXL));
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
  atomic_init(&hdr->poisoned_slots_count, 0);
  atomic_init(&hdr->dead_producers_recovered, 0);
  atomic_init(&hdr->dead_consumers_recovered, 0);
  atomic_init(&hdr->leases_revoked_count, 0);
  atomic_init(&hdr->ticket_holes_healed, 0);
}

/**
 * @brief Atomically increment participant counters based on active roles.
 */
static void rbipc_ring_register_roles(rbipc_shm_header_t *hdr, uint32_t roles) {
  if (!hdr)
    return;
  if (roles & RBIPC_ROLE_PRODUCER) {
    atomic_fetch_add_explicit(&hdr->active_producers, 1, memory_order_relaxed);
  }
  if (roles & RBIPC_ROLE_CONSUMER) {
    atomic_fetch_add_explicit(&hdr->active_consumers, 1, memory_order_relaxed);
  }
}

/**
 * @brief Atomically decrement participant counters based on active roles.
 */
static void rbipc_ring_deregister_roles(rbipc_shm_header_t *hdr,
                                        uint32_t roles) {
  if (!hdr)
    return;
  if (roles & RBIPC_ROLE_PRODUCER) {
    atomic_fetch_sub_explicit(&hdr->active_producers, 1, memory_order_relaxed);
  }
  if (roles & RBIPC_ROLE_CONSUMER) {
    atomic_fetch_sub_explicit(&hdr->active_consumers, 1, memory_order_relaxed);
  }
}

/**
 * @brief Maps both metadata control region and single circular data buffer.
 */
static int rbipc_ring_map_regions(int fd, const rbipc_layout_t *layout,
                                  void **out_ctrl_map, void **out_data_map) {
  void *ctrl_map = NULL;
  int rc = rbipc_vmem_map_ctrl(fd, layout->data_offset, &ctrl_map);
  if (RBIPC_UNLIKELY(rc != RBIPC_OK)) {
    return rc;
  }

  void *data_map = NULL;
  rc = rbipc_vmem_map_data(fd, layout->data_offset, layout->data_size,
                           &data_map);
  if (RBIPC_UNLIKELY(rc != RBIPC_OK)) {
    rbipc_vmem_unmap_ctrl(ctrl_map, layout->data_offset);
    return rc;
  }

  *out_ctrl_map = ctrl_map;
  *out_data_map = data_map;
  return RBIPC_OK;
}

rbipc_ring_t *rbipc_ring_alloc_handle(int fd, const char *RBIPC_RESTRICT name,
                                      rbipc_shm_header_t *hdr,
                                      rbipc_slot_t *slots, void *ctrl_map,
                                      size_t ctrl_map_size, void *data_map,
                                      size_t data_size, uint32_t roles,
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
  if (hdr) {
    ring->cached_capacity_mask = hdr->capacity_mask;
    ring->cached_capacity = hdr->capacity;
    ring->cached_slot_size = hdr->slot_size;
  }
  ring->event_fd = -1;
  ring->owns_eventfd = false;
  ring->wait_policy = RBIPC_WAIT_ADAPTIVE;
  uint32_t hdr_flags = hdr ? le32toh(hdr->flags) : 0;
  ring->is_trusted = (hdr_flags & RBIPC_FLAG_TRUSTED) != 0;
  ring->is_packed = (hdr_flags & RBIPC_FLAG_PACKED) != 0;
  ring->is_hugetlb = (hdr_flags & RBIPC_FLAG_HUGETLB) != 0;
  ring->is_wait_free = (hdr_flags & RBIPC_FLAG_WAIT_FREE) != 0;
  ring->is_isolated = (hdr_flags & RBIPC_FLAG_ISOLATED) != 0;
  ring->is_cxl = (hdr_flags & RBIPC_FLAG_CXL) != 0;
  ring->data_pkey = -1;
  ring->ctrl_pkey = -1;
  ring->help_slot_index = UINT32_MAX;

  if (ring->is_wait_free && ring->hdr && ring->slots) {
    ring->help_slots = (rbipc_help_slot_t *)((char *)ring->slots +
                                             ((size_t)ring->cached_capacity * sizeof(rbipc_slot_t)));
    int wf_rc = rbipc_waitfree_register_peer(ring);
    if (RBIPC_UNLIKELY(wf_rc != RBIPC_OK)) {
      free(ring);
      return NULL;
    }
  }

  if (ring->is_isolated) {
    int mpk_rc = rbipc_mpk_init(ring);
    if (RBIPC_UNLIKELY(mpk_rc != RBIPC_OK)) {
      if (ring->is_wait_free) {
        rbipc_waitfree_unregister_peer(ring);
      }
      free(ring);
      return NULL;
    }
  }

  rbipc_sync_init_process();

  return ring;
}

static void rbipc_ring_abort_create(int fd, const char *name, void *ctrl_map,
                                    size_t ctrl_size, void *data_map,
                                    size_t data_size) {
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

/* ============================================================================
 * Public Creation and Attachment Lifecycle API
 * ============================================================================
 */

/**
 * @brief Allocate, map, and initialize a complete ring buffer instance.
 */
int rbipc_ring_init_and_format_fd(int fd, const char *name, size_t capacity,
                                  uint32_t slot_size, uint32_t flags,
                                  uint32_t roles, size_t page_size,
                                  rbipc_ring_t **out_ring) {
  if (RBIPC_UNLIKELY(!out_ring || capacity == 0 || slot_size == 0)) {
    return RBIPC_ERR_INVAL;
  }
  size_t effective_page = page_size;
  uint32_t cap = 0;
  int rc = rbipc_ring_validate_create_params(capacity, slot_size,
                                             &effective_page, &cap);
  if (RBIPC_UNLIKELY(rc != RBIPC_OK)) {
    return rc;
  }
  effective_page = page_size;

  rbipc_layout_t layout;
  rc = rbipc_shm_calc_layout_ex(cap, slot_size, effective_page, flags, &layout);
  if (RBIPC_UNLIKELY(rc != RBIPC_OK)) {
    return rc;
  }

  void *ctrl_map = NULL;
  void *data_map = NULL;
  rc = rbipc_ring_map_regions(fd, &layout, &ctrl_map, &data_map);
  if (RBIPC_UNLIKELY(rc != RBIPC_OK)) {
    rbipc_ring_abort_create(fd, name, NULL, 0, NULL, 0);
    return rc;
  }

  rbipc_shm_header_t *hdr = (rbipc_shm_header_t *)ctrl_map;
  rbipc_ring_init_header_fields(hdr, &layout, cap, roles, flags);

  rbipc_slot_t *slots =
      (rbipc_slot_t *)((char *)ctrl_map + sizeof(rbipc_shm_header_t));
  rbipc_slot_init_table_ex(slots, cap, (flags & RBIPC_FLAG_PACKED) != 0);

  if (flags & RBIPC_FLAG_WAIT_FREE) {
    rbipc_help_slot_t *help_slots =
        (rbipc_help_slot_t *)((char *)slots +
                              ((size_t)cap * sizeof(rbipc_slot_t)));
    rbipc_waitfree_init_table(help_slots, RBIPC_HELP_MAX_PEERS);
  }

  if (flags & RBIPC_FLAG_CXL) {
    char *p = (char *)ctrl_map;
    for (size_t off = 0; off < layout.data_offset; off += RBIPC_CACHE_LINE) {
      rbipc_clwb(p + off);
    }
  }

  rbipc_ring_t *ring = rbipc_ring_alloc_handle(
      fd, name, hdr, slots, ctrl_map, layout.data_offset, data_map,
      layout.data_size, roles, true);
  if (RBIPC_UNLIKELY(!ring)) {
    rbipc_ring_abort_create(fd, name, ctrl_map, layout.data_offset,
                            data_map, layout.data_size);
    return RBIPC_ERR_NOMEM;
  }

  /* Publish magic sentinel as the final step with release semantics */
  atomic_store_explicit(&hdr->magic, htole64(RBIPC_MAGIC),
                        memory_order_release);
  if (flags & RBIPC_FLAG_CXL) {
    rbipc_clwb(&hdr->magic);
  }

  *out_ring = ring;
  return RBIPC_OK;
}

static int
rbipc_ring_create_instance(const char *name, size_t capacity,
                           uint32_t slot_size, uint32_t flags, mode_t mode,
                           uint32_t roles, size_t page_size,
                           rbipc_ring_t **RBIPC_RESTRICT out_ring) {
  size_t effective_page = page_size;
  uint32_t cap = 0;
  int rc = rbipc_ring_validate_create_params(capacity, slot_size,
                                             &effective_page, &cap);
  if (RBIPC_UNLIKELY(rc != RBIPC_OK)) {
    return rc;
  }
  effective_page = page_size;

  rbipc_layout_t layout;
  rc = rbipc_shm_calc_layout_ex(cap, slot_size, effective_page, flags, &layout);
  if (RBIPC_UNLIKELY(rc != RBIPC_OK)) {
    return rc;
  }

  int fd = -1;
  uint32_t shm_flags = flags & (RBIPC_CREATE_OVERWRITE | RBIPC_SHM_ANON |
                                RBIPC_SHM_NAMED | RBIPC_FLAG_HUGETLB);
  rc = rbipc_shm_create_ex(name, layout.total_shm_size, shm_flags, mode, &fd);
  if (RBIPC_UNLIKELY(rc != RBIPC_OK)) {
    return rc;
  }

  rbipc_shm_seal(fd);

  bool is_named = (flags & RBIPC_SHM_NAMED) ||
                  (!(flags & RBIPC_SHM_ANON) && name != NULL);
  const char *effective_name = is_named ? name : NULL;

  return rbipc_ring_init_and_format_fd(fd, effective_name, capacity, slot_size,
                                       flags, roles, effective_page, out_ring);
}

RBIPC_LEAF
int rbipc_create_ex(const char *RBIPC_RESTRICT name, size_t capacity,
                    uint32_t slot_size, uint32_t flags, mode_t mode,
                    rbipc_ring_t **RBIPC_RESTRICT out_ring) {
  if (RBIPC_UNLIKELY(!out_ring)) {
    return RBIPC_ERR_INVAL;
  }
  uint32_t known_flags = RBIPC_CREATE_OVERWRITE | RBIPC_ROLE_PRODUCER |
                         RBIPC_ROLE_CONSUMER | RBIPC_ROLE_MONITOR |
                         RBIPC_SHM_ANON | RBIPC_SHM_NAMED |
                         RBIPC_FLAG_TRUSTED | RBIPC_FLAG_PACKED |
                         RBIPC_FLAG_HUGETLB | RBIPC_FLAG_WAIT_FREE |
                         RBIPC_FLAG_ISOLATED | RBIPC_FLAG_CXL;
  if (RBIPC_UNLIKELY((flags & ~known_flags) != 0)) {
    return RBIPC_ERR_INVAL;
  }
  if (RBIPC_UNLIKELY((flags & RBIPC_SHM_ANON) && (flags & RBIPC_SHM_NAMED))) {
    return RBIPC_ERR_INVAL;
  }
  if (RBIPC_UNLIKELY((flags & RBIPC_FLAG_HUGETLB) && (flags & RBIPC_SHM_NAMED))) {
    return RBIPC_ERR_INVAL;
  }
  if (RBIPC_UNLIKELY((flags & RBIPC_FLAG_PACKED) &&
                     slot_size > RBIPC_PACKED_MAX_LEN)) {
    return RBIPC_ERR_INVAL;
  }
  uint32_t roles =
      flags & (RBIPC_ROLE_PRODUCER | RBIPC_ROLE_CONSUMER | RBIPC_ROLE_MONITOR);
  if (roles == 0) {
    roles = RBIPC_ROLE_PRODUCER;
  }

  /* Handle HugeTLB (2MB huge pages) request with graceful fallback */
  if (flags & RBIPC_FLAG_HUGETLB) {
    if (rbipc_shm_hugetlb_available()) {
      int h_rc = rbipc_ring_create_instance(name, capacity, slot_size, flags,
                                            mode, roles, RBIPC_HUGE_PAGE_2MB,
                                            out_ring);
      if (h_rc == RBIPC_OK) {
        return RBIPC_OK;
      }
    }
    /* Graceful fallback: clear RBIPC_FLAG_HUGETLB and proceed with standard 4KB pages */
    flags &= ~RBIPC_FLAG_HUGETLB;
  }

  return rbipc_ring_create_instance(name, capacity, slot_size, flags, mode,
                                    roles, rbipc_page_size(), out_ring);
}

RBIPC_LEAF
int rbipc_create(const char *RBIPC_RESTRICT name, size_t capacity,
                 uint32_t slot_size, rbipc_ring_t **RBIPC_RESTRICT out_ring) {
  return rbipc_create_ex(name, capacity, slot_size, RBIPC_CREATE_EXCL, 0,
                         out_ring);
}

RBIPC_LEAF
int rbipc_create_anon(size_t capacity, uint32_t slot_size,
                      rbipc_ring_t **RBIPC_RESTRICT out_ring) {
  return rbipc_create_ex(NULL, capacity, slot_size,
                         RBIPC_CREATE_EXCL | RBIPC_SHM_ANON, 0, out_ring);
}

int rbipc_ring_probe_and_validate_header(
    int fd, size_t page_size, rbipc_layout_t *RBIPC_RESTRICT out_layout) {
  void *probe_map = NULL;
  size_t probe_size = page_size;
  int rc = rbipc_vmem_map_ctrl(fd, probe_size, &probe_map);
  if (rc != RBIPC_OK) {
    probe_size = RBIPC_HUGE_PAGE_2MB;
    rc = rbipc_vmem_map_ctrl(fd, probe_size, &probe_map);
    if (RBIPC_UNLIKELY(rc != RBIPC_OK)) {
      return rc;
    }
  }

  const rbipc_shm_header_t *hdr_check = (const rbipc_shm_header_t *)probe_map;
  if (RBIPC_UNLIKELY(!rbipc_header_is_valid(hdr_check))) {
    rbipc_vmem_unmap_ctrl(probe_map, probe_size);
    return RBIPC_ERR_INVAL;
  }

  size_t data_offset = (size_t)hdr_check->data_offset;
  size_t data_size = (size_t)hdr_check->data_size;
  uint32_t cap = hdr_check->capacity;
  uint32_t slot_size = hdr_check->slot_size;
  uint32_t hdr_flags = hdr_check->flags;

  rbipc_vmem_unmap_ctrl(probe_map, probe_size);

  size_t effective_page_size =
      (hdr_flags & RBIPC_FLAG_HUGETLB) ? RBIPC_HUGE_PAGE_2MB : page_size;
  rc = rbipc_shm_calc_layout_ex(cap, slot_size, effective_page_size, hdr_flags, out_layout);
  if (RBIPC_UNLIKELY(rc != RBIPC_OK || out_layout->data_offset != data_offset ||
                     out_layout->data_size != data_size)) {
    return RBIPC_ERR_INVAL;
  }

  return RBIPC_OK;
}

RBIPC_LEAF
int rbipc_attach_fd_ex(int fd, uint32_t role_flags,
                       rbipc_ring_t **RBIPC_RESTRICT out_ring) {
  if (RBIPC_UNLIKELY(!rbipc_is_valid_fd(fd) || !out_ring)) {
    return RBIPC_ERR_INVAL;
  }
  uint32_t known_roles =
      RBIPC_ROLE_PRODUCER | RBIPC_ROLE_CONSUMER | RBIPC_ROLE_MONITOR;
  if (RBIPC_UNLIKELY(role_flags == 0 || (role_flags & ~known_roles) != 0)) {
    return RBIPC_ERR_INVAL;
  }

  size_t page_size = rbipc_page_size();

  rbipc_layout_t layout;
  int rc = rbipc_ring_probe_and_validate_header(fd, page_size, &layout);
  if (RBIPC_UNLIKELY(rc != RBIPC_OK)) {
    return rc;
  }

  void *ctrl_map = NULL;
  void *data_map = NULL;
  rc = rbipc_ring_map_regions(fd, &layout, &ctrl_map, &data_map);
  if (RBIPC_UNLIKELY(rc != RBIPC_OK)) {
    return rc;
  }

  rbipc_shm_header_t *hdr = (rbipc_shm_header_t *)ctrl_map;
  rbipc_slot_t *slots =
      (rbipc_slot_t *)((char *)ctrl_map + sizeof(rbipc_shm_header_t));

  rbipc_ring_t *ring = rbipc_ring_alloc_handle(
      fd, NULL, hdr, slots, ctrl_map, layout.data_offset, data_map,
      layout.data_size, role_flags, false);
  if (RBIPC_UNLIKELY(!ring)) {
    rbipc_vmem_unmap_data(data_map, layout.data_size);
    rbipc_vmem_unmap_ctrl(ctrl_map, layout.data_offset);
    return RBIPC_ERR_NOMEM;
  }

  rbipc_ring_register_roles(hdr, role_flags);

  *out_ring = ring;
  return RBIPC_OK;
}

RBIPC_LEAF
int rbipc_attach_fd(int fd, rbipc_ring_t **RBIPC_RESTRICT out_ring) {
  return rbipc_attach_fd_ex(fd, RBIPC_ROLE_CONSUMER, out_ring);
}

RBIPC_LEAF
int rbipc_attach_ex(const char *RBIPC_RESTRICT name, uint32_t role_flags,
                    rbipc_ring_t **RBIPC_RESTRICT out_ring) {
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

RBIPC_LEAF
int rbipc_attach(const char *RBIPC_RESTRICT name,
                 rbipc_ring_t **RBIPC_RESTRICT out_ring) {
  return rbipc_attach_ex(name, RBIPC_ROLE_CONSUMER, out_ring);
}

RBIPC_LEAF
void rbipc_ring_unmap_regions(rbipc_ring_t *ring) {
  if (!ring)
    return;

  if (ring->data_map && ring->data_size > 0) {
    rbipc_vmem_unmap_data(ring->data_map, ring->data_size);
    ring->data_map = NULL;
  }

  if (ring->ctrl_map && ring->ctrl_map_size > 0) {
    rbipc_vmem_unmap_ctrl(ring->ctrl_map, ring->ctrl_map_size);
    ring->ctrl_map = NULL;
  }
}

RBIPC_LEAF
int rbipc_detach(rbipc_ring_t *ring) {
  if (RBIPC_UNLIKELY(!ring)) {
    return RBIPC_ERR_INVAL;
  }

  rbipc_reactor_cleanup(ring);

  if (ring->is_wait_free) {
    rbipc_waitfree_unregister_peer(ring);
  }
  if (ring->is_isolated) {
    rbipc_mpk_destroy(ring);
  }

  if (ring->hdr) {
    rbipc_ring_deregister_roles(ring->hdr, ring->roles);
  }

  rbipc_ring_unmap_regions(ring);

  if (ring->fd >= 0) {
    rbipc_shm_close(ring->fd);
    ring->fd = -1;
  }

  free(ring);
  return RBIPC_OK;
}

RBIPC_LEAF
int rbipc_destroy(const char *name) { return rbipc_shm_unlink(name); }

RBIPC_LEAF
int rbipc_signal_shutdown(rbipc_ring_t *ring) {
  if (RBIPC_UNLIKELY(!ring || !ring->hdr)) {
    return RBIPC_ERR_INVAL;
  }

  atomic_store_explicit(&ring->hdr->shutdown_flag, 1, memory_order_release);
  rbipc_sync_wake_all(&ring->hdr->futex_seq, &ring->hdr->futex_waiters);
  rbipc_sync_wake_all(&ring->hdr->write_futex_seq, &ring->hdr->write_waiters);
  rbipc_futex_wake(&ring->hdr->shutdown_flag, INT32_MAX);
  rbipc_reactor_signal(ring);
  return RBIPC_OK;
}

RBIPC_LEAF
int rbipc_shutdown(rbipc_ring_t *ring) {
  return rbipc_signal_shutdown(ring);
}

RBIPC_LEAF
uint32_t rbipc_get_slot_size(const rbipc_ring_t *ring) {
  return ring ? ring->cached_slot_size : 0;
}

RBIPC_LEAF
int rbipc_get_fd(const rbipc_ring_t *ring) { return ring ? ring->fd : -1; }

RBIPC_LEAF
bool rbipc_ring_is_trusted(const rbipc_ring_t *ring) {
  return ring != NULL && ring->is_trusted;
}

RBIPC_LEAF
bool rbipc_ring_is_packed(const rbipc_ring_t *ring) {
  return ring != NULL && ring->is_packed;
}

RBIPC_LEAF
bool rbipc_ring_is_hugetlb(const rbipc_ring_t *ring) {
  return ring != NULL && ring->is_hugetlb;
}

RBIPC_LEAF
bool rbipc_ring_is_wait_free(const rbipc_ring_t *ring) {
  return ring != NULL && ring->is_wait_free;
}

RBIPC_LEAF
bool rbipc_ring_is_isolated(const rbipc_ring_t *ring) {
  return ring != NULL && ring->is_isolated;
}

RBIPC_LEAF
int rbipc_get_stats(const rbipc_ring_t *RBIPC_RESTRICT ring,
                    rbipc_stats_t *RBIPC_RESTRICT out_stats) {
  if (RBIPC_UNLIKELY(!ring || !ring->hdr || !out_stats)) {
    return RBIPC_ERR_INVAL;
  }

  rbipc_shm_header_t *hdr = ring->hdr;
  out_stats->total_shm_size = hdr->total_shm_size;
  out_stats->data_size = hdr->data_size;
  out_stats->capacity = hdr->capacity;
  out_stats->slot_size = hdr->slot_size;
  uint64_t wt = atomic_load_explicit(&hdr->write_ticket, memory_order_relaxed);
  uint64_t rt = atomic_load_explicit(&hdr->read_ticket, memory_order_relaxed);
  out_stats->write_ticket = wt;
  out_stats->read_ticket = rt;
  out_stats->active_producers =
      atomic_load_explicit(&hdr->active_producers, memory_order_relaxed);
  out_stats->active_consumers =
      atomic_load_explicit(&hdr->active_consumers, memory_order_relaxed);
  out_stats->futex_waiters =
      atomic_load_explicit(&hdr->futex_waiters, memory_order_relaxed);
  out_stats->write_waiters =
      atomic_load_explicit(&hdr->write_waiters, memory_order_relaxed);
  out_stats->eventfd_waiters =
      atomic_load_explicit(&hdr->eventfd_waiters, memory_order_relaxed);
  out_stats->lease_timeout_ns =
      atomic_load_explicit(&hdr->lease_timeout_ns, memory_order_relaxed);
  out_stats->poisoned_slots_count =
      atomic_load_explicit(&hdr->poisoned_slots_count, memory_order_relaxed);
  out_stats->dead_producers_recovered = atomic_load_explicit(
      &hdr->dead_producers_recovered, memory_order_relaxed);
  out_stats->dead_consumers_recovered = atomic_load_explicit(
      &hdr->dead_consumers_recovered, memory_order_relaxed);
  out_stats->leases_revoked_count =
      atomic_load_explicit(&hdr->leases_revoked_count, memory_order_relaxed);
  out_stats->ticket_holes_healed =
      atomic_load_explicit(&hdr->ticket_holes_healed, memory_order_relaxed);
  out_stats->is_shutdown =
      (atomic_load_explicit(&hdr->shutdown_flag, memory_order_relaxed) != 0);

  uint64_t raw_backlog = (wt > rt) ? (wt - rt) : 0;
  out_stats->backlog = (raw_backlog > (uint64_t)hdr->capacity)
                           ? (uint64_t)hdr->capacity
                           : raw_backlog;

  return RBIPC_OK;
}

RBIPC_LEAF
int rbipc_get_slot_stats(const rbipc_ring_t *RBIPC_RESTRICT ring,
                         rbipc_slot_stats_t *RBIPC_RESTRICT out_slot_stats) {
  if (RBIPC_UNLIKELY(!ring || !ring->hdr || !ring->slots || !out_slot_stats)) {
    return RBIPC_ERR_INVAL;
  }

  uint32_t cap = ring->hdr->capacity;
  memset(out_slot_stats, 0, sizeof(*out_slot_stats));
  out_slot_stats->capacity = cap;

  uint64_t now_ns = rbipc_clock_monotonic_ns();

  /*
   * Microarchitectural Optimization: Loop Unswitching & Branchless Histogramming.
   *
   * 1. Invariant Hoisting: The ring format (packed vs. standard) is constant across the entire
   *    ring buffer. Hoisting `ring->is_packed` outside the loop eliminates up to 1,048,576
   *    redundant branch evaluations per telemetry snapshot.
   * 2. Branchless Histogramming: Rather than evaluating a 5-way `switch (state)` inside a tight
   *    loop, slot lifecycle states [0, 4] are counted via direct array index increments:
   *    `state_counts[state & 7U]++`. This converts 5 conditional branch paths into a single ALU
   *    load/add/store, completely eliminating branch misprediction penalties.
   */
  uint32_t state_counts[8] = {0};

  if (ring->is_packed) {
    for (uint32_t i = 0; i < cap; ++i) {
      rbipc_slot_t *slot = &ring->slots[i];
      uint32_t state = rbipc_slot_unpack_state(
          atomic_load_explicit(&slot->sequence, memory_order_relaxed));
      state_counts[state & 7U]++;

      uint32_t cpid =
          atomic_load_explicit(&slot->consumer_pid, memory_order_relaxed);
      if (cpid > 0) {
        uint64_t acq_ts = atomic_load_explicit(&slot->acquire_timestamp_ns,
                                               memory_order_relaxed);
        if (acq_ts > 0) {
          out_slot_stats->active_leased_slots++;
          if (now_ns > acq_ts) {
            uint64_t elapsed = now_ns - acq_ts;
            if (elapsed > out_slot_stats->max_lease_elapsed_ns) {
              out_slot_stats->max_lease_elapsed_ns = elapsed;
            }
          }
        }
      }
    }
  } else {
    for (uint32_t i = 0; i < cap; ++i) {
      rbipc_slot_t *slot = &ring->slots[i];
      uint32_t state = atomic_load_explicit(&slot->state, memory_order_relaxed);
      state_counts[state & 7U]++;

      uint32_t cpid =
          atomic_load_explicit(&slot->consumer_pid, memory_order_relaxed);
      if (cpid > 0) {
        uint64_t acq_ts = atomic_load_explicit(&slot->acquire_timestamp_ns,
                                               memory_order_relaxed);
        if (acq_ts > 0) {
          out_slot_stats->active_leased_slots++;
          if (now_ns > acq_ts) {
            uint64_t elapsed = now_ns - acq_ts;
            if (elapsed > out_slot_stats->max_lease_elapsed_ns) {
              out_slot_stats->max_lease_elapsed_ns = elapsed;
            }
          }
        }
      }
    }
  }

  out_slot_stats->empty_count = state_counts[RBIPC_SLOT_EMPTY];
  out_slot_stats->reserving_count = state_counts[RBIPC_SLOT_RESERVING];
  out_slot_stats->reserved_count = state_counts[RBIPC_SLOT_RESERVED];
  out_slot_stats->committed_count = state_counts[RBIPC_SLOT_COMMITTED];
  out_slot_stats->poisoned_count = state_counts[RBIPC_SLOT_POISONED];

  return RBIPC_OK;
}

RBIPC_LEAF
int rbipc_set_wait_policy(rbipc_ring_t *ring, uint32_t policy) {
  if (RBIPC_UNLIKELY(!ring)) {
    return RBIPC_ERR_INVAL;
  }
  uint32_t valid_mask =
      RBIPC_WAIT_ZERO_SPIN | RBIPC_WAIT_ADAPTIVE | RBIPC_WAIT_HW_MONITOR;
  if (RBIPC_UNLIKELY((policy & ~valid_mask) != 0)) {
    return RBIPC_ERR_INVAL;
  }
  ring->wait_policy = policy;
  return RBIPC_OK;
}

RBIPC_LEAF
uint32_t rbipc_get_wait_policy(const rbipc_ring_t *ring) {
  if (RBIPC_UNLIKELY(!ring)) {
    return RBIPC_WAIT_ADAPTIVE;
  }
  return ring->wait_policy;
}

RBIPC_LEAF
int rbipc_set_lease_timeout(rbipc_ring_t *ring, uint64_t lease_timeout_ns) {
  if (RBIPC_UNLIKELY(!ring || !ring->hdr)) {
    return RBIPC_ERR_INVAL;
  }
  atomic_store_explicit(&ring->hdr->lease_timeout_ns, lease_timeout_ns,
                        memory_order_release);
  return RBIPC_OK;
}

RBIPC_LEAF
uint64_t rbipc_get_lease_timeout(const rbipc_ring_t *ring) {
  if (RBIPC_UNLIKELY(!ring || !ring->hdr)) {
    return 0;
  }
  return atomic_load_explicit(&ring->hdr->lease_timeout_ns,
                              memory_order_acquire);
}

RBIPC_LEAF
int rbipc_revoke_expired_leases(rbipc_ring_t *ring) {
  if (RBIPC_UNLIKELY(!ring || !ring->hdr || !ring->slots)) {
    return RBIPC_ERR_INVAL;
  }
  return rbipc_audit_revoke_expired_leases(ring->hdr, ring->slots);
}
