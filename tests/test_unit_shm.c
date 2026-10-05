/**
 * @file test_unit_shm.c
 * @brief Unit tests for shared memory layouts, file descriptors, and sealing
 */

#include "rbipc.h"
#include "../src/rbipc_shm.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>

static void test_layout_calculation(void) {
    rbipc_layout_t layout;

    /* Invalid parameters */
    assert(rbipc_shm_calc_layout(0, 64, 4096, &layout) == RBIPC_ERR_INVAL);
    assert(rbipc_shm_calc_layout(1, 64, 4096, &layout) == RBIPC_ERR_INVAL);
    assert(rbipc_shm_calc_layout(3, 64, 4096, &layout) == RBIPC_ERR_INVAL); /* Not pow 2 */
    assert(rbipc_shm_calc_layout(100, 64, 4096, &layout) == RBIPC_ERR_INVAL);
    assert(rbipc_shm_calc_layout(64, 0, 4096, &layout) == RBIPC_ERR_INVAL);
    assert(rbipc_shm_calc_layout(64, 64, 0, &layout) == RBIPC_ERR_INVAL);
    assert(rbipc_shm_calc_layout(64, 64, 4096, NULL) == RBIPC_ERR_INVAL);

    /* Huge values / overflow */
    assert(rbipc_shm_calc_layout(1024, UINT32_MAX, 4096, &layout) == RBIPC_ERR_OVERFLOW);
    assert(rbipc_shm_calc_layout(1024, UINT32_MAX - 10, 4096, &layout) == RBIPC_ERR_OVERFLOW);

    /* Valid layout */
    int rc = rbipc_shm_calc_layout(1024, 256, 4096, &layout);
    assert(rc == RBIPC_OK);
    assert(layout.capacity == 1024);
    assert(layout.aligned_slot_size == 256);
    assert(layout.data_offset % 4096 == 0);
    assert(layout.data_size % 4096 == 0);
    assert(layout.total_shm_size == layout.data_offset + layout.data_size);
}

static void test_shm_lifecycle(void) {
    int fd = -1;

    /* Invalid arguments */
    assert(rbipc_shm_create("/valid_name", 0, &fd) == RBIPC_ERR_INVAL);
    assert(rbipc_shm_create("/valid_name", 4096, NULL) == RBIPC_ERR_INVAL);
    assert(rbipc_shm_create("invalid_no_slash", 4096, &fd) == RBIPC_ERR_INVAL);
    assert(rbipc_shm_open(NULL, &fd) == RBIPC_ERR_INVAL);
    assert(rbipc_shm_open("no_slash", &fd) == RBIPC_ERR_INVAL);
    assert(rbipc_shm_unlink(NULL) == RBIPC_ERR_INVAL);
    assert(rbipc_shm_unlink("no_slash") == RBIPC_ERR_INVAL);
    assert(rbipc_shm_seal(-1) == RBIPC_ERR_INVAL);

    /* Anonymous memfd creation */
    int rc = rbipc_shm_create(NULL, 8192, &fd);
    assert(rc == RBIPC_OK);
    assert(fd >= 0);
    assert(rbipc_shm_seal(fd) == RBIPC_OK);
    rbipc_shm_close(fd);

    /* Named shm creation */
    const char *test_name = "/rbipc_shm_unit_test";
    rbipc_shm_unlink(test_name);

    rc = rbipc_shm_create(test_name, 16384, &fd);
    assert(rc == RBIPC_OK);
    assert(fd >= 0);

    /* Open existing */
    int fd_opened = -1;
    rc = rbipc_shm_open(test_name, &fd_opened);
    assert(rc == RBIPC_OK);
    assert(fd_opened >= 0);

    rbipc_shm_close(fd_opened);
    rbipc_shm_close(fd);

    /* Unlink */
    assert(rbipc_shm_unlink(test_name) == RBIPC_OK);

    /* Second unlink should fail with SYS */
    assert(rbipc_shm_unlink(test_name) == RBIPC_ERR_SYS);

    /* Open non-existent should fail with SYS */
    assert(rbipc_shm_open(test_name, &fd) == RBIPC_ERR_SYS);
}

int main(void) {
    printf("[test_unit_shm] Running tests...\n");
    test_layout_calculation();
    test_shm_lifecycle();
    printf("[test_unit_shm] All tests passed!\n");
    return 0;
}
