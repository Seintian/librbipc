/**
 * @file test_unit_lifecycle.c
 * @brief Unit tests for ring lifecycle, parameter validation, and attachment
 */

#include "rbipc.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

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
    pwrite(fd, &bad_magic, sizeof(bad_magic), 0);

    /* Attempt to attach to corrupted shm should fail with RBIPC_ERR_INVAL */
    rbipc_ring_t *corrupt_attach = NULL;
    assert(rbipc_attach(TEST_LIFECYCLE_NAME, &corrupt_attach) == RBIPC_ERR_INVAL);

    /* Restore magic, but corrupt data_offset */
    uint64_t good_magic = RBIPC_MAGIC;
    pwrite(fd, &good_magic, sizeof(good_magic), 0);
    uint64_t bad_offset = 12345;
    pwrite(fd, &bad_offset, sizeof(bad_offset), offsetof(rbipc_shm_header_t, data_offset));

    assert(rbipc_attach(TEST_LIFECYCLE_NAME, &corrupt_attach) == RBIPC_ERR_INVAL);

    /* Corrupt header CRC explicitly */
    uint32_t bad_crc = 0xDEADBEEFU;
    pwrite(fd, &bad_crc, sizeof(bad_crc), offsetof(rbipc_shm_header_t, header_crc));
    assert(rbipc_attach(TEST_LIFECYCLE_NAME, &corrupt_attach) == RBIPC_ERR_INVAL);
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

    /* Second creation on existing name without OVERWRITE must fail with RBIPC_ERR_EXISTS */
    rbipc_ring_t *ring2 = NULL;
    rc = rbipc_create(TEST_LIFECYCLE_NAME, 32, 256, &ring2);
    assert(rc == RBIPC_ERR_EXISTS);
    assert(ring2 == NULL);

    /* Explicit EXCL flag must also fail with RBIPC_ERR_EXISTS */
    rc = rbipc_create_ex(TEST_LIFECYCLE_NAME, 32, 256, RBIPC_CREATE_EXCL, 0, &ring2);
    assert(rc == RBIPC_ERR_EXISTS);
    assert(ring2 == NULL);

    /* Invalid flags must fail with RBIPC_ERR_INVAL */
    rc = rbipc_create_ex(TEST_LIFECYCLE_NAME, 32, 256, 0xFFFFU, 0, &ring2);
    assert(rc == RBIPC_ERR_INVAL);
    assert(ring2 == NULL);

    /* OVERWRITE flag must succeed, unlinking and recreating the shared segment */
    rc = rbipc_create_ex(TEST_LIFECYCLE_NAME, 32, 256, RBIPC_CREATE_OVERWRITE, 0, &ring2);
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
    int rc = rbipc_create_ex(TEST_LIFECYCLE_NAME, 32, 256, RBIPC_ROLE_MONITOR, 0, &supervisor);
    assert(rc == RBIPC_OK);
    assert(supervisor != NULL);

    rbipc_stats_t stats;
    assert(rbipc_get_stats(supervisor, &stats) == RBIPC_OK);
    assert(stats.active_producers == 0);
    assert(stats.active_consumers == 0);

    /* 2. Worker 1 attaches as PRODUCER */
    rbipc_ring_t *prod = NULL;
    assert(rbipc_attach_ex(TEST_LIFECYCLE_NAME, RBIPC_ROLE_PRODUCER, &prod) == RBIPC_OK);
    assert(rbipc_get_stats(supervisor, &stats) == RBIPC_OK);
    assert(stats.active_producers == 1);
    assert(stats.active_consumers == 0);

    /* 3. Worker 2 attaches as CONSUMER */
    rbipc_ring_t *cons = NULL;
    assert(rbipc_attach_ex(TEST_LIFECYCLE_NAME, RBIPC_ROLE_CONSUMER, &cons) == RBIPC_OK);
    assert(rbipc_get_stats(supervisor, &stats) == RBIPC_OK);
    assert(stats.active_producers == 1);
    assert(stats.active_consumers == 1);

    /* 4. Worker 3 attaches as MONITOR */
    rbipc_ring_t *mon = NULL;
    assert(rbipc_attach_ex(TEST_LIFECYCLE_NAME, RBIPC_ROLE_MONITOR, &mon) == RBIPC_OK);
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

int main(void) {
    printf("[test_unit_lifecycle] Running tests...\n");
    test_param_validation();
    test_anonymous_memfd_lifecycle();
    test_named_shm_lifecycle();
    test_corrupted_shm_detection();
    test_shm_creation_flags_and_collision();
    test_decoupled_participant_roles();
    printf("[test_unit_lifecycle] All tests passed!\n");
    return 0;
}
