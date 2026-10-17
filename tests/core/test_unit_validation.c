/**
 * @file test_unit_validation.c
 * @brief Comprehensive defensive parameter, error boundary, and corner-case
 *        validation tests for librbipc public API interfaces.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "rbipc.h"
#include "rbipc_shm.h"
#include "rbipc_vmem.h"
#include "rbipc_reactor.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_create_and_attach_validation(void) {
    rbipc_ring_t *ring = NULL;

    /* Capacity validation */
    assert(rbipc_create(NULL, 0, 64, &ring) == RBIPC_ERR_INVAL);
    assert(rbipc_create(NULL, (size_t)RBIPC_MAX_CAPACITY + 1, 64, &ring) == RBIPC_ERR_OVERFLOW);
    assert(rbipc_create(NULL, SIZE_MAX, 64, &ring) == RBIPC_ERR_OVERFLOW);

    /* Slot size validation */
    assert(rbipc_create(NULL, 64, 0, &ring) == RBIPC_ERR_INVAL);
    assert(rbipc_create(NULL, 64, UINT32_MAX, &ring) == RBIPC_ERR_INVAL);

    /* Null output pointer */
    assert(rbipc_create(NULL, 64, 64, NULL) == RBIPC_ERR_INVAL);

    /* Invalid POSIX names */
    assert(rbipc_create("", 64, 64, &ring) == RBIPC_ERR_INVAL);
    assert(rbipc_create("missing_slash", 64, 64, &ring) == RBIPC_ERR_INVAL);
    assert(rbipc_create("//double_slash", 64, 64, &ring) == RBIPC_ERR_INVAL);
    assert(rbipc_create("/", 64, 64, &ring) == RBIPC_ERR_INVAL);

    /* Invalid creation flags */
    assert(rbipc_create_ex(NULL, 64, 64, 0xFFFFFFFFU, 0660, &ring) == RBIPC_ERR_INVAL);

    /* Attach invalid arguments */
    assert(rbipc_attach(NULL, &ring) == RBIPC_ERR_INVAL);
    assert(rbipc_attach("invalid_no_slash", &ring) == RBIPC_ERR_INVAL);
    assert(rbipc_attach("/valid_name", NULL) == RBIPC_ERR_INVAL);
    assert(rbipc_attach("/non_existent_shm_ring_rbipc_xyz", &ring) == RBIPC_ERR_SYS);

    /* Attach fd invalid arguments */
    assert(rbipc_attach_fd(-1, &ring) == RBIPC_ERR_INVAL);
    assert(rbipc_attach_fd(0, NULL) == RBIPC_ERR_INVAL);

    /* Detach and destroy invalid arguments */
    assert(rbipc_detach(NULL) == RBIPC_ERR_INVAL);
    assert(rbipc_destroy(NULL) == RBIPC_ERR_INVAL);
    assert(rbipc_destroy("no_slash") == RBIPC_ERR_INVAL);
    assert(rbipc_destroy("//double_slash") == RBIPC_ERR_INVAL);
    assert(rbipc_destroy("/") == RBIPC_ERR_INVAL);
    assert(rbipc_signal_shutdown(NULL) == RBIPC_ERR_INVAL);
}

static void test_layout_and_vmem_validation(void) {
    rbipc_layout_t layout;

    /* shm layout calculation */
    assert(rbipc_shm_calc_layout(0, 64, 4096, &layout) == RBIPC_ERR_INVAL);
    assert(rbipc_shm_calc_layout(3, 64, 4096, &layout) == RBIPC_ERR_INVAL);
    assert(rbipc_shm_calc_layout(65, 64, 4096, &layout) == RBIPC_ERR_INVAL);
    assert(rbipc_shm_calc_layout(64, 0, 4096, &layout) == RBIPC_ERR_INVAL);
    assert(rbipc_shm_calc_layout(64, 64, 0, &layout) == RBIPC_ERR_INVAL);
    assert(rbipc_shm_calc_layout(64, 64, 4096, NULL) == RBIPC_ERR_INVAL);

    /* Overflow calculation */
    assert(rbipc_shm_calc_layout(1024, UINT32_MAX, 4096, &layout) == RBIPC_ERR_OVERFLOW);

    /* Valid layout calculation check */
    assert(rbipc_shm_calc_layout(128, 256, 4096, &layout) == RBIPC_OK);
    assert(layout.capacity == 128);
    assert(layout.aligned_slot_size == 256);
    assert(layout.data_offset % 4096 == 0);
    assert(layout.data_size % 4096 == 0);
    assert(layout.total_shm_size >= layout.data_offset + layout.data_size);

    /* vmem validation */
    void *map = NULL;
    assert(rbipc_vmem_map_ctrl(-1, 4096, &map) == RBIPC_ERR_INVAL);
    assert(rbipc_vmem_map_ctrl(0, 0, &map) == RBIPC_ERR_INVAL);
    assert(rbipc_vmem_map_ctrl(0, 4096, NULL) == RBIPC_ERR_INVAL);

    assert(rbipc_vmem_map_data(-1, 0, 4096, &map) == RBIPC_ERR_INVAL);
    assert(rbipc_vmem_map_data(0, 0, 0, &map) == RBIPC_ERR_INVAL);
    assert(rbipc_vmem_map_data(0, 0, 4096, NULL) == RBIPC_ERR_INVAL);

    assert(rbipc_vmem_map_double(-1, 0, 4096, &map) == RBIPC_ERR_INVAL);
    assert(rbipc_vmem_map_double(0, 0, 0, &map) == RBIPC_ERR_INVAL);
    assert(rbipc_vmem_map_double(0, 0, 4096, NULL) == RBIPC_ERR_INVAL);

    /* Defensive unmap checks (safe no-ops) */
    rbipc_vmem_unmap_ctrl(NULL, 0);
    rbipc_vmem_unmap_ctrl(NULL, 4096);
    rbipc_vmem_unmap_data(NULL, 0);
    rbipc_vmem_unmap_data(NULL, 4096);
    rbipc_vmem_unmap_double(NULL, 0);
    rbipc_vmem_unmap_double(NULL, 4096);
}

static void test_telemetry_and_policy_validation(void) {
    rbipc_stats_t stats;
    rbipc_slot_stats_t slot_stats;

    assert(rbipc_get_stats(NULL, &stats) == RBIPC_ERR_INVAL);
    assert(rbipc_get_slot_stats(NULL, &slot_stats) == RBIPC_ERR_INVAL);
    assert(rbipc_get_wait_policy(NULL) == RBIPC_WAIT_ADAPTIVE);
    assert(rbipc_set_wait_policy(NULL, RBIPC_WAIT_ZERO_SPIN) == RBIPC_ERR_INVAL);
    assert(rbipc_get_lease_timeout(NULL) == 0);
    assert(rbipc_set_lease_timeout(NULL, 1000) == RBIPC_ERR_INVAL);

    /* Test on valid ring */
    rbipc_ring_t *ring = NULL;
    assert(rbipc_create(NULL, 8, 128, &ring) == RBIPC_OK);
    assert(rbipc_get_stats(ring, NULL) == RBIPC_ERR_INVAL);
    assert(rbipc_get_slot_stats(ring, NULL) == RBIPC_ERR_INVAL);

    assert(rbipc_get_stats(ring, &stats) == RBIPC_OK);
    assert(stats.capacity == 8);
    assert(stats.slot_size == 128);

    assert(rbipc_get_slot_stats(ring, &slot_stats) == RBIPC_OK);
    assert(slot_stats.capacity == 8);
    assert(slot_stats.empty_count == 8);

    assert(rbipc_set_wait_policy(ring, RBIPC_WAIT_ZERO_SPIN) == RBIPC_OK);
    assert(rbipc_get_wait_policy(ring) == RBIPC_WAIT_ZERO_SPIN);

    assert(rbipc_set_lease_timeout(ring, 5000000ULL) == RBIPC_OK);
    assert(rbipc_get_lease_timeout(ring) == 5000000ULL);

    rbipc_detach(ring);
}

static void test_reactor_validation(void) {
    int fd = -1;
    assert(rbipc_enable_eventfd(NULL, &fd) == RBIPC_ERR_INVAL);
    assert(rbipc_set_eventfd(NULL, 5) == RBIPC_ERR_INVAL);
    assert(rbipc_get_eventfd(NULL, &fd) == RBIPC_ERR_INVAL);
    assert(rbipc_eventfd_drain(NULL) == RBIPC_ERR_INVAL);

    rbipc_ring_t *ring = NULL;
    assert(rbipc_create(NULL, 8, 128, &ring) == RBIPC_OK);
    assert(rbipc_enable_eventfd(ring, NULL) == RBIPC_ERR_INVAL);
    assert(rbipc_set_eventfd(ring, -1) == RBIPC_ERR_INVAL);
    assert(rbipc_get_eventfd(ring, NULL) == RBIPC_ERR_INVAL);

    /* Before enabling, querying fd should return RBIPC_ERR_NOTSUP */
    assert(rbipc_get_eventfd(ring, &fd) == RBIPC_ERR_NOTSUP);
    assert(rbipc_eventfd_drain(ring) == RBIPC_ERR_NOTSUP);

    rbipc_detach(ring);
}

static void test_strerror_coverage(void) {
    const int codes[] = {
        RBIPC_OK, RBIPC_ERR_INVAL, RBIPC_ERR_NOMEM, RBIPC_ERR_SYS,
        RBIPC_ERR_FULL, RBIPC_ERR_EMPTY, RBIPC_ERR_POISONED, RBIPC_ERR_SHUTDOWN,
        RBIPC_ERR_TIMEOUT, RBIPC_ERR_BUSY, RBIPC_ERR_OVERFLOW, RBIPC_ERR_EXISTS,
        RBIPC_ERR_NOTSUP, -999, 999
    };
    const size_t num_codes = sizeof(codes) / sizeof(codes[0]);
    for (size_t i = 0; i < num_codes; ++i) {
        const char *msg = rbipc_strerror(codes[i]);
        assert(msg != NULL);
        assert(strlen(msg) > 0);
    }
}

int main(void) {
    printf("[test_unit_validation] Running defensive validation and boundary tests...\n");
    test_create_and_attach_validation();
    test_layout_and_vmem_validation();
    test_telemetry_and_policy_validation();
    test_reactor_validation();
    test_strerror_coverage();
    printf("[test_unit_validation] All tests passed!\n");
    return 0;
}
