/**
 * @file test_unit_dma.c
 * @brief Unit tests for hardware-assisted DMA offload engine (Intel DSA) and streaming fallback.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "rbipc.h"
#include "rbipc_io.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DMA_RING_CAPACITY 16U
#define DMA_RING_SLOT_SIZE 16384U

static void test_dma_probes(void) {
  bool dma_supported = rbipc_dma_available();
  printf("  [probe] Intel Data Streaming Accelerator (DSA) available: %s\n",
         dma_supported ? "YES (/dev/dsa work queues active)" : "NO (using vector streaming fallback)");
}

static void test_dma_param_validation(void) {
  rbipc_ring_t *ring = NULL;
  int rc = rbipc_create(NULL, DMA_RING_CAPACITY, DMA_RING_SLOT_SIZE, &ring);
  assert(rc == RBIPC_OK);

  char buf[64] = {0};
  assert(rbipc_write_dma(NULL, buf, sizeof(buf)) == RBIPC_ERR_INVAL);
  assert(rbipc_write_dma(ring, NULL, sizeof(buf)) == RBIPC_ERR_INVAL);
  assert(rbipc_write_dma(ring, buf, 0) == RBIPC_ERR_INVAL);

  assert(rbipc_write_dma_timeout(NULL, buf, sizeof(buf), 1000) == RBIPC_ERR_INVAL);
  assert(rbipc_write_dma_timeout(ring, NULL, sizeof(buf), 1000) == RBIPC_ERR_INVAL);
  assert(rbipc_write_dma_timeout(ring, buf, 0, 1000) == RBIPC_ERR_INVAL);

  rbipc_detach(ring);
}

static void test_dma_small_payload(void) {
  rbipc_ring_t *ring = NULL;
  int rc = rbipc_create(NULL, DMA_RING_CAPACITY, DMA_RING_SLOT_SIZE, &ring);
  assert(rc == RBIPC_OK);

  const char msg[] = "Small DMA test message";
  rc = rbipc_write_dma(ring, msg, sizeof(msg));
  assert(rc == RBIPC_OK);

  char recv_buf[128] = {0};
  uint32_t bytes_read = 0;
  rc = rbipc_read(ring, recv_buf, sizeof(recv_buf), &bytes_read);
  assert(rc == RBIPC_OK);
  assert(bytes_read == sizeof(msg));
  assert(strcmp(recv_buf, msg) == 0);

  rbipc_detach(ring);
}

static void test_dma_large_payload(void) {
  rbipc_ring_t *ring = NULL;
  int rc = rbipc_create(NULL, DMA_RING_CAPACITY, DMA_RING_SLOT_SIZE, &ring);
  assert(rc == RBIPC_OK);

  size_t large_len = 8192;
  char *large_src = (char *)malloc(large_len);
  char *large_dst = (char *)malloc(large_len);
  assert(large_src && large_dst);

  for (size_t i = 0; i < large_len; ++i) {
    large_src[i] = (char)(i & 0xFF);
  }

  rc = rbipc_write_dma(ring, large_src, large_len);
  assert(rc == RBIPC_OK);

  uint32_t bytes_read = 0;
  rc = rbipc_read(ring, large_dst, large_len, &bytes_read);
  assert(rc == RBIPC_OK);
  assert(bytes_read == large_len);
  assert(memcmp(large_src, large_dst, large_len) == 0);

  /* Test with timeout */
  rc = rbipc_write_dma_timeout(ring, large_src, large_len, 1000000000ULL);
  assert(rc == RBIPC_OK);

  bytes_read = 0;
  rc = rbipc_read(ring, large_dst, large_len, &bytes_read);
  assert(rc == RBIPC_OK);
  assert(bytes_read == large_len);
  assert(memcmp(large_src, large_dst, large_len) == 0);

  free(large_src);
  free(large_dst);
  rbipc_detach(ring);
}

int main(void) {
  printf("[test_unit_dma] Running Intel DSA hardware DMA offload tests...\n");

  test_dma_probes();
  test_dma_param_validation();
  test_dma_small_payload();
  test_dma_large_payload();

  printf("[test_unit_dma] All DMA offload tests passed successfully.\n");
  return 0;
}
