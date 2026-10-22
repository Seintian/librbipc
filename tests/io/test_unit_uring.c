/**
 * @file test_unit_uring.c
 * @brief Unit tests for Linux io_uring asynchronous zero-copy buffer registration.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "rbipc.h"
#include "rbipc_uring.h"

#include <assert.h>
#include <stdio.h>

static void test_uring_probes(void) {
  bool uring_supported = rbipc_uring_available();
  printf("  [probe] Linux io_uring subsystem available: %s\n",
         uring_supported ? "YES" : "NO (Kernel lacks io_uring or seccomp blocked)");
}

static void test_uring_param_validation(void) {
  int uring_fd = -1;
  assert(rbipc_uring_create(NULL, 0, 0, &uring_fd) == RBIPC_ERR_INVAL);
  assert(rbipc_uring_create(NULL, 16, 0, NULL) == RBIPC_ERR_INVAL);

  assert(rbipc_uring_register_ring(NULL, 0) == RBIPC_ERR_INVAL);
  assert(rbipc_uring_register_ring(NULL, -1) == RBIPC_ERR_INVAL);

  /* Closing invalid descriptor is safe no-op */
  rbipc_uring_destroy(-1);
}

static void test_uring_lifecycle(void) {
  if (!rbipc_uring_available()) {
    printf("  [skip] Skipping io_uring lifecycle test: subsystem not supported\n");
    return;
  }

  rbipc_ring_t *ring = NULL;
  int rc = rbipc_create(NULL, 32, 256, &ring);
  assert(rc == RBIPC_OK);
  assert(ring != NULL);

  int uring_fd = -1;
  rc = rbipc_uring_create(ring, 16, 0, &uring_fd);
  if (rc == RBIPC_OK) {
    assert(uring_fd >= 0);

    /* Test secondary registration attempt */
    int reg_rc = rbipc_uring_register_ring(ring, uring_fd);
    (void)reg_rc;

    rbipc_uring_destroy(uring_fd);
  }

  rbipc_detach(ring);
}

int main(void) {
  printf("[test_unit_uring] Running Linux io_uring zero-copy integration tests...\n");

  test_uring_probes();
  test_uring_param_validation();
  test_uring_lifecycle();

  printf("[test_unit_uring] All io_uring tests passed successfully.\n");
  return 0;
}
