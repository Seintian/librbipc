/**
 * @file test_unit_lifecycle.c
 * @brief Unit tests for ring lifecycle, parameter validation, and attachment
 */

#include "rbipc.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define TEST_LIFECYCLE_NAME "/rbipc_lifecycle_test"

static void test_param_validation(void) {
  rbipc_ring_t *ring = NULL;

  /* Invalid arguments */
  assert(rbipc_create(NULL, 0, 64, &ring) == RBIPC_ERR_INVAL);
  assert(rbipc_create(NULL, 64, 0, &ring) == RBIPC_ERR_INVAL);
  assert(rbipc_create(NULL, 64, 64, NULL) == RBIPC_ERR_INVAL);
  assert(rbipc_create("no_leading_slash", 64, 64, &ring) == RBIPC_ERR_INVAL);

  /* Overflow capacity */
  assert(rbipc_create(NULL, 0x80000001U, 64, &ring) == RBIPC_ERR_OVERFLOW);

  /* Attach validation */
  assert(rbipc_attach(NULL, &ring) == RBIPC_ERR_INVAL);
  assert(rbipc_attach("no_slash", &ring) == RBIPC_ERR_INVAL);
  assert(rbipc_attach(TEST_LIFECYCLE_NAME, NULL) == RBIPC_ERR_INVAL);
  assert(rbipc_attach_fd(-1, &ring) == RBIPC_ERR_INVAL);
  assert(rbipc_attach_fd(0, NULL) == RBIPC_ERR_INVAL);

  /* Detach and destroy validation */
  assert(rbipc_detach(NULL) == RBIPC_ERR_INVAL);
  assert(rbipc_destroy(NULL) == RBIPC_ERR_INVAL);
  assert(rbipc_destroy("no_slash") == RBIPC_ERR_INVAL);

  /* Stats and fd validation */
  assert(rbipc_get_fd(NULL) == -1);
  rbipc_stats_t stats;
  assert(rbipc_get_stats(NULL, &stats) == RBIPC_ERR_INVAL);
  assert(rbipc_signal_shutdown(NULL) == RBIPC_ERR_INVAL);
}

static void test_anonymous_memfd_lifecycle(void) {
  rbipc_ring_t *ring = NULL;
  int rc = rbipc_create(NULL, 16, 128, &ring);
  assert(rc == RBIPC_OK);
  assert(ring != NULL);

  int fd = rbipc_get_fd(ring);
  assert(fd >= 0);

  rbipc_stats_t stats;
  rc = rbipc_get_stats(ring, &stats);
  assert(rc == RBIPC_OK);
  assert(stats.capacity == 16);
  assert(stats.slot_size >= 128);
  assert(stats.active_producers == 1);
  assert(stats.active_consumers == 0);
  assert(stats.is_shutdown == false);

  /* Attach via open fd */
  rbipc_ring_t *attached = NULL;
  rc = rbipc_attach_fd(fd, &attached);
  assert(rc == RBIPC_OK);
  assert(attached != NULL);

  rc = rbipc_get_stats(ring, &stats);
  assert(rc == RBIPC_OK);
  assert(stats.active_consumers == 1);

  rc = rbipc_detach(attached);
  assert(rc == RBIPC_OK);

  rc = rbipc_get_stats(ring, &stats);
  assert(rc == RBIPC_OK);
  assert(stats.active_consumers == 0);

  rc = rbipc_detach(ring);
  assert(rc == RBIPC_OK);
}

static void test_named_shm_lifecycle(void) {
  rbipc_destroy(TEST_LIFECYCLE_NAME);

  rbipc_ring_t *creator = NULL;
  int rc = rbipc_create(TEST_LIFECYCLE_NAME, 32, 256, &creator);
  assert(rc == RBIPC_OK);
  assert(creator != NULL);

  rbipc_ring_t *consumer = NULL;
  rc = rbipc_attach(TEST_LIFECYCLE_NAME, &consumer);
  assert(rc == RBIPC_OK);
  assert(consumer != NULL);

  rbipc_stats_t stats;
  rc = rbipc_get_stats(consumer, &stats);
  assert(rc == RBIPC_OK);
  assert(stats.active_producers == 1);
  assert(stats.active_consumers == 1);

  rc = rbipc_detach(consumer);
  assert(rc == RBIPC_OK);

  rc = rbipc_detach(creator);
  assert(rc == RBIPC_OK);

  rc = rbipc_destroy(TEST_LIFECYCLE_NAME);
  assert(rc == RBIPC_OK);
}

static void test_corrupted_shm_detection(void) {
  rbipc_destroy(TEST_LIFECYCLE_NAME);

  rbipc_ring_t *ring = NULL;
  int rc = rbipc_create(TEST_LIFECYCLE_NAME, 16, 64, &ring);
  assert(rc == RBIPC_OK);

  /* Corrupt the magic */
  int fd = shm_open(TEST_LIFECYCLE_NAME, O_RDWR, RBIPC_SHM_MODE);
  assert(fd >= 0);
  uint64_t bad_magic = 0xDEADBEEF;
  ssize_t pw = pwrite(fd, &bad_magic, sizeof(bad_magic), 0);
  assert(pw == (ssize_t)sizeof(bad_magic));

  /* Attempt to attach to corrupted shm should fail with RBIPC_ERR_INVAL */
  rbipc_ring_t *corrupt_attach = NULL;
  assert(rbipc_attach(TEST_LIFECYCLE_NAME, &corrupt_attach) == RBIPC_ERR_INVAL);

  /* Restore magic, but corrupt data_offset */
  uint64_t good_magic = RBIPC_MAGIC;
  pw = pwrite(fd, &good_magic, sizeof(good_magic), 0);
  assert(pw == (ssize_t)sizeof(good_magic));
  uint64_t bad_offset = 12345;
  pw = pwrite(fd, &bad_offset, sizeof(bad_offset),
              offsetof(rbipc_shm_header_t, data_offset));
  assert(pw == (ssize_t)sizeof(bad_offset));

  assert(rbipc_attach(TEST_LIFECYCLE_NAME, &corrupt_attach) == RBIPC_ERR_INVAL);

  /* Corrupt header CRC explicitly */
  uint32_t bad_crc = 0xDEADBEEFU;
  pw = pwrite(fd, &bad_crc, sizeof(bad_crc),
              offsetof(rbipc_shm_header_t, header_crc));
  assert(pw == (ssize_t)sizeof(bad_crc));
  assert(rbipc_attach(TEST_LIFECYCLE_NAME, &corrupt_attach) == RBIPC_ERR_INVAL);
  (void)pw;
  close(fd);

  /* Test attaching invalid device fd like /dev/null */
  int null_fd = open("/dev/null", O_RDWR);
  if (null_fd >= 0) {
    assert(rbipc_attach_fd(null_fd, &corrupt_attach) != RBIPC_OK);
    close(null_fd);
  }

  rbipc_detach(ring);
  rbipc_destroy(TEST_LIFECYCLE_NAME);
}

static void test_shm_creation_flags_and_collision(void) {
  rbipc_destroy(TEST_LIFECYCLE_NAME);

  rbipc_ring_t *ring1 = NULL;
  int rc = rbipc_create(TEST_LIFECYCLE_NAME, 32, 256, &ring1);
  assert(rc == RBIPC_OK);
  assert(ring1 != NULL);

  /* Second creation on existing name without OVERWRITE must fail with
   * RBIPC_ERR_EXISTS */
  rbipc_ring_t *ring2 = NULL;
  rc = rbipc_create(TEST_LIFECYCLE_NAME, 32, 256, &ring2);
  assert(rc == RBIPC_ERR_EXISTS);
  assert(ring2 == NULL);

  /* Explicit EXCL flag must also fail with RBIPC_ERR_EXISTS */
  rc = rbipc_create_ex(TEST_LIFECYCLE_NAME, 32, 256, RBIPC_CREATE_EXCL, 0,
                       &ring2);
  assert(rc == RBIPC_ERR_EXISTS);
  assert(ring2 == NULL);

  /* Invalid flags must fail with RBIPC_ERR_INVAL */
  rc = rbipc_create_ex(TEST_LIFECYCLE_NAME, 32, 256, 0xFFFFU, 0, &ring2);
  assert(rc == RBIPC_ERR_INVAL);
  assert(ring2 == NULL);

  /* OVERWRITE flag must succeed, unlinking and recreating the shared segment */
  rc = rbipc_create_ex(TEST_LIFECYCLE_NAME, 32, 256, RBIPC_CREATE_OVERWRITE, 0,
                       &ring2);
  assert(rc == RBIPC_OK);
  assert(ring2 != NULL);

  /* Clean up */
  rbipc_detach(ring1);
  rbipc_detach(ring2);
  rbipc_destroy(TEST_LIFECYCLE_NAME);
}

static void test_decoupled_participant_roles(void) {
  rbipc_destroy(TEST_LIFECYCLE_NAME);

  /* 1. Supervisor creates ring buffer with RBIPC_ROLE_MONITOR */
  rbipc_ring_t *supervisor = NULL;
  int rc = rbipc_create_ex(TEST_LIFECYCLE_NAME, 32, 256, RBIPC_ROLE_MONITOR, 0,
                           &supervisor);
  assert(rc == RBIPC_OK);
  assert(supervisor != NULL);

  rbipc_stats_t stats;
  assert(rbipc_get_stats(supervisor, &stats) == RBIPC_OK);
  assert(stats.active_producers == 0);
  assert(stats.active_consumers == 0);

  /* 2. Worker 1 attaches as PRODUCER */
  rbipc_ring_t *prod = NULL;
  assert(rbipc_attach_ex(TEST_LIFECYCLE_NAME, RBIPC_ROLE_PRODUCER, &prod) ==
         RBIPC_OK);
  assert(rbipc_get_stats(supervisor, &stats) == RBIPC_OK);
  assert(stats.active_producers == 1);
  assert(stats.active_consumers == 0);

  /* 3. Worker 2 attaches as CONSUMER */
  rbipc_ring_t *cons = NULL;
  assert(rbipc_attach_ex(TEST_LIFECYCLE_NAME, RBIPC_ROLE_CONSUMER, &cons) ==
         RBIPC_OK);
  assert(rbipc_get_stats(supervisor, &stats) == RBIPC_OK);
  assert(stats.active_producers == 1);
  assert(stats.active_consumers == 1);

  /* 4. Worker 3 attaches as MONITOR */
  rbipc_ring_t *mon = NULL;
  assert(rbipc_attach_ex(TEST_LIFECYCLE_NAME, RBIPC_ROLE_MONITOR, &mon) ==
         RBIPC_OK);
  assert(rbipc_get_stats(supervisor, &stats) == RBIPC_OK);
  assert(stats.active_producers == 1);
  assert(stats.active_consumers == 1);

  /* 5. Supervisor detaches: active_producers must remain 1 */
  assert(rbipc_detach(supervisor) == RBIPC_OK);
  assert(rbipc_get_stats(prod, &stats) == RBIPC_OK);
  assert(stats.active_producers == 1);
  assert(stats.active_consumers == 1);

  /* 6. Detach prod, cons, mon */
  assert(rbipc_detach(prod) == RBIPC_OK);
  assert(rbipc_get_stats(cons, &stats) == RBIPC_OK);
  assert(stats.active_producers == 0);
  assert(stats.active_consumers == 1);

  assert(rbipc_detach(cons) == RBIPC_OK);
  assert(rbipc_get_stats(mon, &stats) == RBIPC_OK);
  assert(stats.active_producers == 0);
  assert(stats.active_consumers == 0);

  assert(rbipc_detach(mon) == RBIPC_OK);
  assert(rbipc_destroy(TEST_LIFECYCLE_NAME) == RBIPC_OK);
}

static void test_shm_type_selection(void) {
  rbipc_ring_t *ring = NULL;

  /* 1. Default choice when name is NULL: Anonymous memfd */
  int rc = rbipc_create(NULL, 16, 64, &ring);
  assert(rc == RBIPC_OK);
  assert(ring != NULL);
  assert(rbipc_get_fd(ring) >= 0);
  assert(rbipc_detach(ring) == RBIPC_OK);

  /* 2. Empty string without RBIPC_SHM_ANON fails POSIX name validation */
  assert(rbipc_create("", 16, 64, &ring) == RBIPC_ERR_INVAL);

  /* 3. Explicit rbipc_create_anon helper */
  rc = rbipc_create_anon(16, 64, &ring);
  assert(rc == RBIPC_OK);
  assert(ring != NULL);
  assert(rbipc_get_fd(ring) >= 0);
  assert(rbipc_detach(ring) == RBIPC_OK);

  /* 4. Explicit RBIPC_SHM_ANON flag */
  rc = rbipc_create_ex(NULL, 16, 64, RBIPC_SHM_ANON, 0, &ring);
  assert(rc == RBIPC_OK);
  assert(rbipc_detach(ring) == RBIPC_OK);

  /* 5. Explicit RBIPC_SHM_ANON with a name (debug name in memfd, not in
   * /dev/shm) */
  rc = rbipc_create_ex("/my_anon_debug", 16, 64, RBIPC_SHM_ANON, 0, &ring);
  assert(rc == RBIPC_OK);
  rbipc_ring_t *att = NULL;
  /* Named attach to /my_anon_debug must fail because it was created anonymous
   */
  assert(rbipc_attach("/my_anon_debug", &att) != RBIPC_OK);
  assert(rbipc_detach(ring) == RBIPC_OK);

  /* 6. Invalid: RBIPC_SHM_NAMED with NULL name */
  assert(rbipc_create_ex(NULL, 16, 64, RBIPC_SHM_NAMED, 0, &ring) ==
         RBIPC_ERR_INVAL);

  /* 7. Invalid: Mutually exclusive RBIPC_SHM_ANON | RBIPC_SHM_NAMED */
  assert(rbipc_create_ex("/test_conflict", 16, 64,
                         RBIPC_SHM_ANON | RBIPC_SHM_NAMED, 0,
                         &ring) == RBIPC_ERR_INVAL);

  /* 8. Explicit RBIPC_SHM_NAMED with valid name */
  rbipc_destroy(TEST_LIFECYCLE_NAME);
  rc = rbipc_create_ex(TEST_LIFECYCLE_NAME, 16, 64, RBIPC_SHM_NAMED, 0, &ring);
  assert(rc == RBIPC_OK);
  assert(rbipc_attach(TEST_LIFECYCLE_NAME, &att) == RBIPC_OK);
  assert(rbipc_detach(att) == RBIPC_OK);
  assert(rbipc_detach(ring) == RBIPC_OK);
  assert(rbipc_destroy(TEST_LIFECYCLE_NAME) == RBIPC_OK);
}

static void test_trusted_and_packed_mode_lifecycle(void) {
  rbipc_ring_t *ring = NULL;

  /* 1. Tier 2: RBIPC_FLAG_TRUSTED */
  rbipc_destroy(TEST_LIFECYCLE_NAME);
  int rc = rbipc_create_ex(TEST_LIFECYCLE_NAME, 32, 128,
                           RBIPC_SHM_NAMED | RBIPC_FLAG_TRUSTED, 0, &ring);
  assert(rc == RBIPC_OK);
  assert(ring != NULL);
  assert(rbipc_ring_is_trusted(ring) == true);
  assert(rbipc_ring_is_packed(ring) == false);

  /* Verify write and read work correctly in trusted mode */
  const char *msg1 = "trusted_mode_test";
  assert(rbipc_write(ring, msg1, (uint32_t)strlen(msg1) + 1) == RBIPC_OK);
  const void *rbuf = NULL;
  uint32_t rlen = 0;
  uint64_t rticket = 0;
  assert(rbipc_read_acquire(ring, &rbuf, &rlen, &rticket) == RBIPC_OK);
  assert(strcmp((const char *)rbuf, msg1) == 0);
  assert(rbipc_read_release(ring, rticket) == RBIPC_OK);

  /* Verify attached handle inherits trusted mode */
  rbipc_ring_t *att = NULL;
  assert(rbipc_attach(TEST_LIFECYCLE_NAME, &att) == RBIPC_OK);
  assert(rbipc_ring_is_trusted(att) == true);
  assert(rbipc_ring_is_packed(att) == false);
  assert(rbipc_detach(att) == RBIPC_OK);
  assert(rbipc_detach(ring) == RBIPC_OK);
  assert(rbipc_destroy(TEST_LIFECYCLE_NAME) == RBIPC_OK);

  /* 2. Tier 3: RBIPC_FLAG_PACKED */
  assert(rbipc_create_ex(TEST_LIFECYCLE_NAME, 32, 128,
                         RBIPC_SHM_NAMED | RBIPC_FLAG_PACKED, 0, &ring) == RBIPC_OK);
  assert(ring != NULL);
  assert(rbipc_ring_is_trusted(ring) == false);
  assert(rbipc_ring_is_packed(ring) == true);

  /* Verify write and read work correctly in packed mode */
  const char *msg2 = "packed_mode_test";
  assert(rbipc_write(ring, msg2, (uint32_t)strlen(msg2) + 1) == RBIPC_OK);
  assert(rbipc_read_acquire(ring, &rbuf, &rlen, &rticket) == RBIPC_OK);
  assert(strcmp((const char *)rbuf, msg2) == 0);
  assert(rbipc_read_release(ring, rticket) == RBIPC_OK);

  /* Verify attached handle inherits packed mode */
  assert(rbipc_attach(TEST_LIFECYCLE_NAME, &att) == RBIPC_OK);
  assert(rbipc_ring_is_trusted(att) == false);
  assert(rbipc_ring_is_packed(att) == true);
  assert(rbipc_detach(att) == RBIPC_OK);
  assert(rbipc_detach(ring) == RBIPC_OK);
  assert(rbipc_destroy(TEST_LIFECYCLE_NAME) == RBIPC_OK);

  /* 3. Combined Tier 2 + Tier 3: RBIPC_FLAG_TRUSTED | RBIPC_FLAG_PACKED */
  assert(rbipc_create_ex(NULL, 32, 128,
                         RBIPC_FLAG_TRUSTED | RBIPC_FLAG_PACKED, 0, &ring) == RBIPC_OK);
  assert(ring != NULL);
  assert(rbipc_ring_is_trusted(ring) == true);
  assert(rbipc_ring_is_packed(ring) == true);

  const char *msg3 = "trusted_and_packed";
  assert(rbipc_write(ring, msg3, (uint32_t)strlen(msg3) + 1) == RBIPC_OK);
  assert(rbipc_read_acquire(ring, &rbuf, &rlen, &rticket) == RBIPC_OK);
  assert(strcmp((const char *)rbuf, msg3) == 0);
  assert(rbipc_read_release(ring, rticket) == RBIPC_OK);
  assert(rbipc_detach(ring) == RBIPC_OK);

  /* 4. Invalid configuration: packed mode with slot size exceeding 4095 */
  assert(rbipc_create_ex(NULL, 16, RBIPC_PACKED_MAX_LEN + 1,
                         RBIPC_FLAG_PACKED, 0, &ring) == RBIPC_ERR_INVAL);
}

int main(void) {
  printf("[test_unit_lifecycle] Running tests...\n");
  test_param_validation();
  test_anonymous_memfd_lifecycle();
  test_shm_type_selection();
  test_trusted_and_packed_mode_lifecycle();
  test_named_shm_lifecycle();
  test_corrupted_shm_detection();
  test_shm_creation_flags_and_collision();
  test_decoupled_participant_roles();
  printf("[test_unit_lifecycle] All tests passed successfully.\n");
  return 0;
}
