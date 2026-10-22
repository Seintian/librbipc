/**
 * @file test_unit_waitfree.c
 * @brief Unit and concurrency tests for Bounded Wait-Free Helping Engine (Nikolaev wCQ).
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "rbipc.h"
#include "rbipc_internal.h"
#include "rbipc_waitfree.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define WF_TEST_CAPACITY 64U
#define WF_TEST_SLOT_SIZE 128U
#define WF_TEST_MSGS 5000U
#define WF_NUM_PRODUCERS 4
#define WF_NUM_CONSUMERS 2

typedef struct {
  rbipc_ring_t *ring;
  int thread_id;
  uint32_t count;
} wf_thread_arg_t;

static void *wf_producer_thread(void *arg) {
  wf_thread_arg_t *targ = (wf_thread_arg_t *)arg;
  rbipc_ring_t *ring = targ->ring;
  char buf[64];

  for (uint32_t i = 0; i < targ->count; ++i) {
    snprintf(buf, sizeof(buf), "p%d_msg_%u", targ->thread_id, i);
    size_t len = strlen(buf) + 1;
    int rc = rbipc_write(ring, buf, len);
    assert(rc == RBIPC_OK);
  }
  return NULL;
}

static void *wf_consumer_thread(void *arg) {
  wf_thread_arg_t *targ = (wf_thread_arg_t *)arg;
  rbipc_ring_t *ring = targ->ring;
  char buf[128];

  for (uint32_t i = 0; i < targ->count; ++i) {
    uint32_t bytes_read = 0;
    int rc = rbipc_read(ring, buf, sizeof(buf), &bytes_read);
    assert(rc == RBIPC_OK);
    assert(bytes_read > 0);
  }
  return NULL;
}

static void test_waitfree_lifecycle(void) {
  rbipc_ring_t *ring = NULL;
  uint32_t flags = RBIPC_ROLE_PRODUCER | RBIPC_ROLE_CONSUMER | RBIPC_FLAG_WAIT_FREE | RBIPC_SHM_ANON;
  int rc = rbipc_create_ex(NULL, WF_TEST_CAPACITY, WF_TEST_SLOT_SIZE, flags, 0, &ring);
  assert(rc == RBIPC_OK);
  assert(ring != NULL);
  assert(rbipc_ring_is_wait_free(ring) == true);
  assert(ring->help_slots != NULL);
  assert(ring->help_slot_index < RBIPC_HELP_MAX_PEERS);

  /* Verify single message publish & consume */
  const char test_msg[] = "Hello Wait-Free World!";
  rc = rbipc_write(ring, test_msg, sizeof(test_msg));
  assert(rc == RBIPC_OK);

  char recv_buf[128] = {0};
  uint32_t bytes_read = 0;
  rc = rbipc_read(ring, recv_buf, sizeof(recv_buf), &bytes_read);
  assert(rc == RBIPC_OK);
  assert(bytes_read == sizeof(test_msg));
  assert(strcmp(recv_buf, test_msg) == 0);

  rc = rbipc_detach(ring);
  assert(rc == RBIPC_OK);
}

static void test_waitfree_cooperative_slow_path(void) {
  rbipc_ring_t *ring = NULL;
  uint32_t flags = RBIPC_ROLE_PRODUCER | RBIPC_ROLE_CONSUMER | RBIPC_FLAG_WAIT_FREE | RBIPC_SHM_ANON;
  int rc = rbipc_create_ex(NULL, WF_TEST_CAPACITY, WF_TEST_SLOT_SIZE, flags, 0, &ring);
  assert(rc == RBIPC_OK);

  /* Directly test rbipc_waitfree_post_and_wait for write operation */
  const char slow_msg[] = "Cooperative Helping Payload";
  void *slot_buf = NULL;
  uint64_t ticket = 0;
  rc = rbipc_reserve_write(ring, sizeof(slow_msg), &slot_buf, &ticket);
  assert(rc == RBIPC_OK);

  rc = rbipc_waitfree_post_and_wait(ring, ticket, RBIPC_HELP_OP_WRITE,
                                   slow_msg, sizeof(slow_msg), 500000000ULL);
  assert(rc == RBIPC_OK);

  /* Consume and verify */
  char read_buf[64] = {0};
  uint32_t read_len = 0;
  rc = rbipc_read(ring, read_buf, sizeof(read_buf), &read_len);
  assert(rc == RBIPC_OK);
  assert(strcmp(read_buf, slow_msg) == 0);

  /* Test READ cooperative helping slow path */
  const char read_help_msg[] = "Read Helping Test";
  rc = rbipc_write(ring, read_help_msg, sizeof(read_help_msg));
  assert(rc == RBIPC_OK);
  rc = rbipc_waitfree_post_and_wait(ring, ticket + 1, RBIPC_HELP_OP_READ, NULL, 0, 500000000ULL);
  assert(rc == RBIPC_OK);
  rc = rbipc_read(ring, read_buf, sizeof(read_buf), &read_len);
  assert(rc == RBIPC_OK);
  assert(strcmp(read_buf, read_help_msg) == 0);

  rc = rbipc_detach(ring);
  assert(rc == RBIPC_OK);
}

static void test_waitfree_concurrent_stress(void) {
  rbipc_ring_t *ring = NULL;
  uint32_t flags = RBIPC_ROLE_PRODUCER | RBIPC_ROLE_CONSUMER | RBIPC_FLAG_WAIT_FREE | RBIPC_SHM_ANON;
  int rc = rbipc_create_ex(NULL, WF_TEST_CAPACITY, WF_TEST_SLOT_SIZE, flags, 0, &ring);
  assert(rc == RBIPC_OK);

  pthread_t producers[WF_NUM_PRODUCERS];
  pthread_t consumers[WF_NUM_CONSUMERS];
  wf_thread_arg_t prod_args[WF_NUM_PRODUCERS];
  wf_thread_arg_t cons_args[WF_NUM_CONSUMERS];

  uint32_t msgs_per_prod = WF_TEST_MSGS;
  uint32_t total_msgs = msgs_per_prod * WF_NUM_PRODUCERS;
  uint32_t msgs_per_cons = total_msgs / WF_NUM_CONSUMERS;

  for (int i = 0; i < WF_NUM_CONSUMERS; ++i) {
    cons_args[i].ring = ring;
    cons_args[i].thread_id = i;
    cons_args[i].count = msgs_per_cons;
    pthread_create(&consumers[i], NULL, wf_consumer_thread, &cons_args[i]);
  }

  for (int i = 0; i < WF_NUM_PRODUCERS; ++i) {
    prod_args[i].ring = ring;
    prod_args[i].thread_id = i;
    prod_args[i].count = msgs_per_prod;
    pthread_create(&producers[i], NULL, wf_producer_thread, &prod_args[i]);
  }

  for (int i = 0; i < WF_NUM_PRODUCERS; ++i) {
    pthread_join(producers[i], NULL);
  }
  for (int i = 0; i < WF_NUM_CONSUMERS; ++i) {
    pthread_join(consumers[i], NULL);
  }

  rbipc_stats_t stats;
  rc = rbipc_get_stats(ring, &stats);
  assert(rc == RBIPC_OK);
  assert(stats.write_ticket == total_msgs);
  assert(stats.read_ticket == total_msgs);

  rc = rbipc_detach(ring);
  assert(rc == RBIPC_OK);
}

int main(void) {
  printf("[test_unit_waitfree] Running Nikolaev wCQ bounded wait-free helping tests...\n");

  test_waitfree_lifecycle();
  test_waitfree_cooperative_slow_path();
  test_waitfree_concurrent_stress();

  printf("[test_unit_waitfree] All wait-free tests passed successfully.\n");
  return 0;
}
