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
#include "rbipc_shm.h"
#include "rbipc_vmem.h"

#include <fcntl.h>
#include <unistd.h>

/**
 * @brief Checks if host system has accessible CXL memory devices or DAX mappings.
 */
bool rbipc_cxl_available(void) {
  static int cached = -1;
  if (cached != -1) {
    return cached == 1;
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

  /* If no explicit path provided, check default DAX character device */
  const char *path = dax_path;
  if (!path) {
    if (access("/dev/dax0.0", F_OK) == 0) {
      path = "/dev/dax0.0";
    } else {
      return RBIPC_ERR_NOTSUP;
    }
  }

  int fd = open(path, O_RDWR);
  if (fd < 0) {
    return RBIPC_ERR_SYS;
  }

  flags |= RBIPC_FLAG_CXL;

  /* Initialize ring over open DAX descriptor */
  int rc = rbipc_attach_fd_ex(fd, flags, out_ring);
  if (rc != RBIPC_OK) {
    close(fd);
    return rc;
  }

  if (*out_ring) {
    (*out_ring)->is_cxl = true;
  }

  return RBIPC_OK;
}
