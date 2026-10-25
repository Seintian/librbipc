/**
 * @file rbipc_cxl.c
 * @brief Implementation of Compute Express Link (CXL) coherent memory fabric integration.
 *
 * @details Detects CXL bus topology, creates shared memory rings over DAX devices,
 *          and applies cache-line writeback (CLWB) to ensure low-latency snoop coherency.
 *
 * @author Seintian
 * @date 2026
 * @copyright Apache License 2.0
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "rbipc.h"
#include "rbipc_internal.h"
#include "rbipc_cxl.h"
#include "rbipc_ring.h"
#include "rbipc_shm.h"
#include "rbipc_vmem.h"
#include "rbipc_math.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>

/**
 * @brief Checks if host system has accessible CXL memory devices or DAX mappings.
 */
bool rbipc_cxl_available(void) {
  static int cached = -1;
  if (cached != -1) {
    return cached == 1;
  }

  const char *env = getenv("RBIPC_CXL_DAX_PATH");
  if (env && env[0] != 0 && access(env, F_OK) == 0) {
    cached = 1;
    return true;
  }
  if (access("/sys/bus/cxl", F_OK) == 0 || access("/dev/dax0.0", F_OK) == 0) {
    cached = 1;
    return true;
  }

  cached = 0;
  return false;
}

/**
 * @brief Checks whether the ring operates over a CXL disaggregated memory fabric.
 */
RBIPC_LEAF
bool rbipc_ring_is_cxl(const rbipc_ring_t *ring) {
  return (ring != NULL) && ring->is_cxl;
}

/**
 * @brief Creates a shared memory ring mapped over a persistent CXL DAX device.
 */
int rbipc_create_cxl(const char *dax_path, size_t capacity, size_t slot_size,
                     uint32_t flags, rbipc_ring_t **out_ring) {
  if (RBIPC_UNLIKELY(!out_ring || capacity == 0 || slot_size == 0)) {
    return RBIPC_ERR_INVAL;
  }
  if (RBIPC_UNLIKELY(!rbipc_is_power_of_two(capacity) ||
                     capacity < RBIPC_MIN_CAPACITY ||
                     capacity > RBIPC_MAX_CAPACITY)) {
    return RBIPC_ERR_INVAL;
  }

  /* If no explicit path provided, check environment variable or default DAX character device */
  const char *path = dax_path;
  if (!path) {
    const char *env = getenv("RBIPC_CXL_DAX_PATH");
    if (env && env[0] != 0) {
      path = env;
    } else if (access("/dev/dax0.0", F_OK) == 0) {
      path = "/dev/dax0.0";
    } else {
      return RBIPC_ERR_NOTSUP;
    }
  }

  int fd = open(path, O_RDWR | O_CREAT, 0660);
  if (fd < 0) {
    return RBIPC_ERR_SYS;
  }

  flags |= RBIPC_FLAG_CXL;
  uint32_t roles = flags & (RBIPC_ROLE_PRODUCER | RBIPC_ROLE_CONSUMER | RBIPC_ROLE_MONITOR);
  if (roles == 0) {
    roles = RBIPC_ROLE_PRODUCER | RBIPC_ROLE_CONSUMER;
  }

  size_t page_sz = rbipc_page_size();
  rbipc_layout_t layout;
  int rc = rbipc_shm_calc_layout_ex((uint32_t)capacity, (uint32_t)slot_size, page_sz, flags, &layout);
  if (RBIPC_UNLIKELY(rc != RBIPC_OK)) {
    close(fd);
    return rc;
  }

  struct stat st;
  if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) {
    if ((size_t)st.st_size < layout.total_shm_size) {
      if (ftruncate(fd, (off_t)layout.total_shm_size) != 0) {
        close(fd);
        return RBIPC_ERR_SYS;
      }
    }
  }

  rc = rbipc_ring_init_and_format_fd(fd, path, capacity, (uint32_t)slot_size,
                                     flags, roles, page_sz, out_ring);
  if (rc != RBIPC_OK) {
    close(fd);
    return rc;
  }

  return RBIPC_OK;
}
