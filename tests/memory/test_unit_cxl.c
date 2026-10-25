/**
 * @file test_unit_cxl.c
 * @brief Unit tests for Compute Express Link (CXL) coherent memory and cache-line writeback.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "rbipc.h"
#include "rbipc_cxl.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

static void test_cxl_probes(void) {
  bool cxl_supported = rbipc_cxl_available();
  printf("  [probe] CXL memory topology / DAX device present: %s\n",
         cxl_supported ? "YES (/dev/dax or /sys/bus/cxl active)" : "NO (Commodore / Non-CXL platform)");
}

static void test_clwb_execution(void) {
  /* Allocate memory aligned to 64 bytes and test CLWB execution */
  void *mem = NULL;
  int rc = posix_memalign(&mem, 64, 512);
  assert(rc == 0 && mem != NULL);

  memset(mem, 0x42, 512);

  /* Issue CLWB on each cache line */
  char *ptr = (char *)mem;
  for (size_t offset = 0; offset < 512; offset += 64) {
    rbipc_clwb(ptr + offset);
  }

  free(mem);
}

static void test_cxl_ring_attributes(void) {
  rbipc_ring_t *ring = NULL;
  int rc = rbipc_create(NULL, 16, 64, &ring);
  assert(rc == RBIPC_OK);
  assert(ring != NULL);

  /* Standard ring should not report as CXL */
  assert(rbipc_ring_is_cxl(ring) == false);

  rbipc_detach(ring);
}

static void test_cxl_create_validation(void) {
  rbipc_ring_t *ring = NULL;

  /* Invalid arguments */
  assert(rbipc_create_cxl(NULL, 0, 64, 0, &ring) == RBIPC_ERR_INVAL);
  assert(rbipc_create_cxl(NULL, 16, 0, 0, &ring) == RBIPC_ERR_INVAL);
  assert(rbipc_create_cxl(NULL, 16, 64, 0, NULL) == RBIPC_ERR_INVAL);

  /* Non-existent DAX path */
  assert(rbipc_create_cxl("/dev/dax999.999", 16, 64, 0, &ring) == RBIPC_ERR_SYS);
}

static void test_cxl_flag_execution(void) {
  rbipc_ring_t *ring = NULL;
  uint32_t flags = RBIPC_ROLE_PRODUCER | RBIPC_ROLE_CONSUMER | RBIPC_FLAG_CXL | RBIPC_SHM_ANON;
  int rc = rbipc_create_ex(NULL, 16, 64, flags, 0, &ring);
  assert(rc == RBIPC_OK);
  assert(ring != NULL);
  assert(rbipc_ring_is_cxl(ring) == true);

  const char msg[] = "CXL CLWB Test Message";
  rc = rbipc_write(ring, msg, sizeof(msg));
  assert(rc == RBIPC_OK);

  char recv_buf[64] = {0};
  uint32_t len = 0;
  rc = rbipc_read(ring, recv_buf, sizeof(recv_buf), &len);
  assert(rc == RBIPC_OK);
  assert(len == sizeof(msg));
  assert(strcmp(recv_buf, msg) == 0);

  rbipc_detach(ring);
}


static void test_cxl_file_backed_creation(void) {
  const char *mock_path = "/tmp/test_mock_cxl_dax.raw";
  unlink(mock_path);

  rbipc_ring_t *ring = NULL;
  int rc = rbipc_create_cxl(mock_path, 32, 128, RBIPC_ROLE_PRODUCER | RBIPC_ROLE_CONSUMER, &ring);
  assert(rc == RBIPC_OK);
  assert(ring != NULL);
  assert(rbipc_ring_is_cxl(ring) == true);

  const char test_data[] = "CXL DAX Persisted Message Payload";
  rc = rbipc_write(ring, test_data, sizeof(test_data));
  assert(rc == RBIPC_OK);

  char recv_buf[128] = {0};
  uint32_t len = 0;
  rc = rbipc_read(ring, recv_buf, sizeof(recv_buf), &len);
  assert(rc == RBIPC_OK);
  assert(len == sizeof(test_data));
  assert(strcmp(recv_buf, test_data) == 0);

  rc = rbipc_detach(ring);
  assert(rc == RBIPC_OK);

  /* Now attach back to verify that the DAX file has valid librbipc header and metadata */
  int fd = open(mock_path, O_RDWR);
  assert(fd >= 0);

  rbipc_ring_t *ring_attached = NULL;
  rc = rbipc_attach_fd_ex(fd, RBIPC_ROLE_PRODUCER | RBIPC_ROLE_CONSUMER, &ring_attached);
  assert(rc == RBIPC_OK);
  assert(ring_attached != NULL);

  rc = rbipc_detach(ring_attached);
  assert(rc == RBIPC_OK);
  close(fd);
  unlink(mock_path);
}

int main(void) {
  printf("[test_unit_cxl] Running Compute Express Link (CXL) tests...\n");

  test_cxl_probes();
  test_clwb_execution();
  test_cxl_ring_attributes();
  test_cxl_create_validation();
  test_cxl_flag_execution();
  test_cxl_file_backed_creation();

  printf("[test_unit_cxl] All CXL tests passed successfully.\n");
  return 0;
}
