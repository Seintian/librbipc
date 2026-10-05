/**
 * @file test_unit_error.c
 * @brief Unit tests for error string descriptions
 */

#include "rbipc.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

int main(void) {
    printf("[test_unit_error] Running tests...\n");

    assert(strcmp(rbipc_strerror(RBIPC_OK), "Success") == 0);
    assert(strcmp(rbipc_strerror(RBIPC_ERR_INVAL), "Invalid argument or configuration") == 0);
    assert(strcmp(rbipc_strerror(RBIPC_ERR_NOMEM), "Memory allocation or mapping failure") == 0);
    assert(strcmp(rbipc_strerror(RBIPC_ERR_SYS), "System call error (inspect errno)") == 0);
    assert(strcmp(rbipc_strerror(RBIPC_ERR_FULL), "Ring buffer is currently full") == 0);
    assert(strcmp(rbipc_strerror(RBIPC_ERR_EMPTY), "Ring buffer is currently empty") == 0);
    assert(strcmp(rbipc_strerror(RBIPC_ERR_POISONED), "Slot producer crashed; slot poisoned") == 0);
    assert(strcmp(rbipc_strerror(RBIPC_ERR_SHUTDOWN), "Ring buffer signaled shutdown") == 0);
    assert(strcmp(rbipc_strerror(RBIPC_ERR_TIMEOUT), "Operation timed out") == 0);
    assert(strcmp(rbipc_strerror(RBIPC_ERR_BUSY), "Resource is busy or in contention") == 0);
    assert(strcmp(rbipc_strerror(RBIPC_ERR_OVERFLOW), "Arithmetic or buffer calculation overflow") == 0);
    assert(strcmp(rbipc_strerror(-999), "Unknown error") == 0);
    assert(strcmp(rbipc_strerror(999), "Unknown error") == 0);

    printf("[test_unit_error] All tests passed!\n");
    return 0;
}
