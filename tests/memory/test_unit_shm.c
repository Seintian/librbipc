/**
 * @file test_unit_shm.c
 * @brief Unit tests for shared memory layouts, file descriptors, and sealing
 */

#include "rbipc.h"
#include "rbipc_shm.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

static void test_layout_calculation(void) {
  rbipc_layout_t layout;

  /* Invalid parameters */
  assert(rbipc_shm_calc_layout(0, 64, 4096, &layout) == RBIPC_ERR_INVAL);
  assert(rbipc_shm_calc_layout(1, 64, 4096, &layout) == RBIPC_ERR_INVAL);
  assert(rbipc_shm_calc_layout(3, 64, 4096, &layout) ==
         RBIPC_ERR_INVAL); /* Not pow 2 */
  assert(rbipc_shm_calc_layout(100, 64, 4096, &layout) == RBIPC_ERR_INVAL);
  assert(rbipc_shm_calc_layout(64, 0, 4096, &layout) == RBIPC_ERR_INVAL);
  assert(rbipc_shm_calc_layout(64, 64, 0, &layout) == RBIPC_ERR_INVAL);
  assert(rbipc_shm_calc_layout(64, 64, 4096, NULL) == RBIPC_ERR_INVAL);

  /* Huge values / overflow */
  assert(rbipc_shm_calc_layout(1024, UINT32_MAX, 4096, &layout) ==
         RBIPC_ERR_OVERFLOW);
  assert(rbipc_shm_calc_layout(1024, UINT32_MAX - 10, 4096, &layout) ==
         RBIPC_ERR_OVERFLOW);

  /* Valid layout */
  int rc = rbipc_shm_calc_layout(1024, 256, 4096, &layout);
  assert(rc == RBIPC_OK);
  assert(layout.capacity == 1024);
  assert(layout.aligned_slot_size == 256);
  assert(layout.data_offset % 4096 == 0);
  assert(layout.data_size % 4096 == 0);
  assert(layout.total_shm_size == layout.data_offset + layout.data_size);
}

static void test_shm_permissions_and_umask(void) {
  const char *test_name = "/rbipc_shm_umask_test";
  rbipc_shm_unlink(test_name);

  /* Set restrictive umask that would normally strip group/other write
   * permissions */
  mode_t old_mask = umask(0077);

  int fd = -1;
  /* Create with explicit 0664 mode */
  int rc = rbipc_shm_create_ex(test_name, 4096, RBIPC_CREATE_EXCL, 0664, &fd);
  assert(rc == RBIPC_OK);
  assert(fd >= 0);

  struct stat st;
  assert(fstat(fd, &st) == 0);
  /* Verify fchmod bypassed the restrictive umask of 0077 */
  assert((st.st_mode & 0777) == 0664);

  rbipc_shm_close(fd);
  rbipc_shm_unlink(test_name);

  /* Test default mode (0) under umask 0077 -> must be RBIPC_DEFAULT_SHM_MODE
   * (0660) */
  rc = rbipc_shm_create_ex(test_name, 4096, RBIPC_CREATE_EXCL, 0, &fd);
  assert(rc == RBIPC_OK);
  assert(fd >= 0);
  assert(fstat(fd, &st) == 0);
  assert((st.st_mode & 0777) == RBIPC_DEFAULT_SHM_MODE);

  rbipc_shm_close(fd);
  rbipc_shm_unlink(test_name);

  /* Restore umask */
  umask(old_mask);
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

  /* Sealing named POSIX shm must return RBIPC_ERR_NOTSUP (Linux tmpfs
   * restriction) */
  assert(rbipc_shm_seal(fd) == RBIPC_ERR_NOTSUP);

  /* Second create without overwrite flag must fail with RBIPC_ERR_EXISTS */
  int fd_coll = -1;
  assert(rbipc_shm_create(test_name, 16384, &fd_coll) == RBIPC_ERR_EXISTS);
  assert(rbipc_shm_create_ex(test_name, 16384, RBIPC_CREATE_EXCL, 0,
                             &fd_coll) == RBIPC_ERR_EXISTS);
  assert(rbipc_shm_create_ex(test_name, 16384, 0x8000, 0, &fd_coll) ==
         RBIPC_ERR_INVAL);
  assert(rbipc_shm_create_ex(NULL, 16384, RBIPC_SHM_NAMED, 0, &fd_coll) ==
         RBIPC_ERR_INVAL);
  assert(rbipc_shm_create_ex(test_name, 16384, RBIPC_SHM_ANON | RBIPC_SHM_NAMED,
                             0, &fd_coll) == RBIPC_ERR_INVAL);

  /* Test explicit anonymous creation with name produces sealable memfd */
  int fd_anon_named = -1;
  assert(rbipc_shm_create_ex(test_name, 16384, RBIPC_SHM_ANON, 0,
                             &fd_anon_named) == RBIPC_OK);
  assert(fd_anon_named >= 0);
  assert(rbipc_shm_seal(fd_anon_named) == RBIPC_OK);
  rbipc_shm_close(fd_anon_named);

  /* Test overwrite flag replaces existing object */
  int fd_over = -1;
  assert(rbipc_shm_create_ex(test_name, 16384, RBIPC_CREATE_OVERWRITE, 0660,
                             &fd_over) == RBIPC_OK);
  assert(fd_over >= 0);
  rbipc_shm_close(fd_over);

  /* Negative fd close should safely no-op */
  rbipc_shm_close(-1);

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
  test_shm_permissions_and_umask();
  printf("[test_unit_shm] All tests passed!\n");
  return 0;
}
