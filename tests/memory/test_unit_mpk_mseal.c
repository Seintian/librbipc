/**
 * @file test_unit_mpk_mseal.c
 * @brief Unit tests for Intel Memory Protection Keys (MPK) and Linux mseal(2) memory sealing.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "rbipc.h"
#include "rbipc_internal.h"
#include "rbipc_mpk.h"
#include "rbipc_vmem.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static void test_availability_probes(void) {
  bool mseal_supported = rbipc_mseal_available();
  bool mpk_supported = rbipc_mpk_available();

  printf("  [probe] Host Linux mseal(2) supported: %s\n",
         mseal_supported ? "YES (Linux 6.10+)" : "NO (Kernel < 6.10 or restricted)");
  printf("  [probe] Host Intel MPK/PKU supported: %s\n",
         mpk_supported ? "YES" : "NO (CPU lacking PKU or CR4.PKE disabled)");
}

static void test_isolated_ring_lifecycle(void) {
  rbipc_ring_t *ring = NULL;
  uint32_t flags = RBIPC_ROLE_PRODUCER | RBIPC_ROLE_CONSUMER | RBIPC_FLAG_ISOLATED | RBIPC_SHM_ANON;
  int rc = rbipc_create_ex(NULL, 32, 128, flags, 0, &ring);
  assert(rc == RBIPC_OK);
  assert(ring != NULL);
  assert(rbipc_ring_is_isolated(ring) == true);

  /* Verify payload writes with hardware MPK permission transitions */
  rbipc_mpk_allow_write(ring);
  rbipc_mpk_deny_write(ring);

  const char test_data[] = "Isolated Zero-Trust Secure Payload";
  rc = rbipc_write(ring, test_data, sizeof(test_data));
  assert(rc == RBIPC_OK);

  char recv_buf[128] = {0};
  uint32_t bytes_read = 0;
  rc = rbipc_read(ring, recv_buf, sizeof(recv_buf), &bytes_read);
  assert(rc == RBIPC_OK);
  assert(bytes_read == sizeof(test_data));
  assert(strcmp(recv_buf, test_data) == 0);

  rc = rbipc_detach(ring);
  assert(rc == RBIPC_OK);
}

static void test_vmem_seal_direct(void) {
  size_t page_sz = (size_t)sysconf(_SC_PAGESIZE);
  void *mem = mmap(NULL, page_sz, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert(mem != MAP_FAILED);

  /* Write initial test pattern */
  memset(mem, 0x5A, page_sz);

  int seal_rc = rbipc_vmem_seal(mem, page_sz);
  if (rbipc_mseal_available()) {
    assert(seal_rc == RBIPC_OK);
    /* In Linux 6.10+, subsequent mprotect calls are blocked by mseal */
    int mprot_rc = mprotect(mem, page_sz, PROT_READ);
    assert(mprot_rc == -1); /* Blocked by kernel mseal */
  } else {
    assert(seal_rc == RBIPC_OK);
  }

  munmap(mem, page_sz);
}

int main(void) {
  printf("[test_unit_mpk_mseal] Running Intel MPK and Linux mseal tests...\n");

  test_availability_probes();
  test_isolated_ring_lifecycle();
  test_vmem_seal_direct();

  printf("[test_unit_mpk_mseal] All MPK and mseal tests passed successfully.\n");
  return 0;
}
