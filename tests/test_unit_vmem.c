/**
 * @file test_unit_vmem.c
 * @brief Unit tests for virtual memory control mapping and double-mapped buffer
 */

#include "rbipc.h"
#include "../src/rbipc_vmem.h"
#include "../src/rbipc_shm.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

static void test_vmem_ctrl_map(void) {
    void *map = NULL;
    assert(rbipc_vmem_map_ctrl(-1, 4096, &map) == RBIPC_ERR_INVAL);
    assert(rbipc_vmem_map_ctrl(0, 0, &map) == RBIPC_ERR_INVAL);
    assert(rbipc_vmem_map_ctrl(0, 4096, NULL) == RBIPC_ERR_INVAL);

    /* Safe unmap on NULL */
    rbipc_vmem_unmap_ctrl(NULL, 0);
    rbipc_vmem_unmap_ctrl(NULL, 4096);
}

static void test_vmem_double_map(void) {
    void *data_map = NULL;
    assert(rbipc_vmem_map_double(-1, 0, 4096, &data_map) == RBIPC_ERR_INVAL);
    assert(rbipc_vmem_map_double(0, 0, 0, &data_map) == RBIPC_ERR_INVAL);
    assert(rbipc_vmem_map_double(0, 0, 4096, NULL) == RBIPC_ERR_INVAL);

    /* Create anonymous memfd */
    int fd = -1;
    size_t data_size = 65536;
    int rc = rbipc_shm_create(NULL, data_size, &fd);
    assert(rc == RBIPC_OK);

    /* Map double virtual ring */
    rc = rbipc_vmem_map_double(fd, 0, data_size, &data_map);
    assert(rc == RBIPC_OK);
    assert(data_map != NULL);

    /* Verify mirroring property!
     * Writing bytes at the start of buffer 1 must immediately be reflected at buffer 2!
     */
    char *buf1 = (char *)data_map;
    char *buf2 = (char *)data_map + data_size;

    const char *test_str = "Double-Mapping-Magic-12345";
    size_t test_len = strlen(test_str) + 1;

    memcpy(buf1, test_str, test_len);
    /* Compiler memory barrier: inform optimizer of external OS virtual memory aliasing */
    __asm__ __volatile__("" ::: "memory");
    assert(memcmp(buf2, test_str, test_len) == 0);

    /* Write at end of buf1 straddling the circular border into buf2 */
    size_t boundary_offset = data_size - 10;
    memcpy(buf1 + boundary_offset, "STRADDLE_OK", 11);
    __asm__ __volatile__("" ::: "memory");
    /* Contiguous reading from buf1 + boundary_offset reads across border seamlessly */
    assert(memcmp(buf1 + boundary_offset, "STRADDLE_OK", 11) == 0);
    assert(buf1[0] == 'K');
    assert(buf2[0] == 'K');

    /* Clean up */
    rbipc_vmem_unmap_double(data_map, data_size);
    rbipc_shm_close(fd);
}

int main(void) {
    printf("[test_unit_vmem] Running tests...\n");
    test_vmem_ctrl_map();
    test_vmem_double_map();
    printf("[test_unit_vmem] All tests passed!\n");
    return 0;
}
