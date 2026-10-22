/**
 * @file test_unit_uintr.c
 * @brief Unit tests for user-space hardware interrupts (Intel UINTR / SENDUIPI).
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "rbipc.h"
#include "rbipc_uintr.h"

#include <assert.h>
#include <stdio.h>

static void test_uintr_probes(void) {
  bool uintr_supported = rbipc_uintr_available();
  printf("  [probe] Intel User Interrupts (UINTR / SENDUIPI) supported: %s\n",
         uintr_supported ? "YES" : "NO (CPU lacking UINTR or kernel support)");
}

static void test_uintr_signaling(void) {
  /* Vector 0 should be rejected defensively */
  assert(rbipc_uintr_signal_peer(0) == false);

  if (rbipc_uintr_available()) {
    /* If supported, test non-zero vector */
    (void)rbipc_uintr_signal_peer(1);
  } else {
    /* On CPUs without UINTR, signal_peer cleanly returns false */
    assert(rbipc_uintr_signal_peer(1) == false);
  }
}

int main(void) {
  printf("[test_unit_uintr] Running user-space hardware interrupt (UINTR) tests...\n");

  test_uintr_probes();
  test_uintr_signaling();

  printf("[test_unit_uintr] All user interrupt tests passed successfully.\n");
  return 0;
}
