/**
 * @file test_unit_error.c
 * @brief Unit tests for error string descriptions
 */

#include "rbipc.h"
#include "rbipc_error.h"
#include <errno.h>
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
    assert(strcmp(rbipc_strerror(RBIPC_ERR_EXISTS), "Shared memory object already exists") == 0);
    assert(strcmp(rbipc_strerror(RBIPC_ERR_NOTSUP), "Operation not supported") == 0);
    assert(strcmp(rbipc_strerror(-999), "Unknown error") == 0);
    assert(strcmp(rbipc_strerror(999), "Unknown error") == 0);

    /* Test rbipc_status_is_error */
    assert(!rbipc_status_is_error(RBIPC_OK));
    assert(rbipc_status_is_error(RBIPC_ERR_INVAL));
    assert(rbipc_status_is_error(RBIPC_ERR_NOMEM));

    /* Test rbipc_errno_to_status */
    assert(rbipc_errno_to_status(0) == RBIPC_OK);
    assert(rbipc_errno_to_status(EINVAL) == RBIPC_ERR_INVAL);
    assert(rbipc_errno_to_status(ENOMEM) == RBIPC_ERR_NOMEM);
    assert(rbipc_errno_to_status(EEXIST) == RBIPC_ERR_EXISTS);
    assert(rbipc_errno_to_status(ETIMEDOUT) == RBIPC_ERR_TIMEOUT);
    assert(rbipc_errno_to_status(EBUSY) == RBIPC_ERR_BUSY);
    assert(rbipc_errno_to_status(EOPNOTSUPP) == RBIPC_ERR_NOTSUP);
    assert(rbipc_errno_to_status(9999) == RBIPC_ERR_SYS);

    /* Test rbipc_error_format */
    char buf[256];
    assert(rbipc_error_format(RBIPC_ERR_EXISTS, 0, buf, sizeof(buf)) == buf);
    assert(strcmp(buf, "Shared memory object already exists") == 0);

    assert(rbipc_error_format(RBIPC_ERR_SYS, ENOENT, buf, sizeof(buf)) == buf);
    assert(strstr(buf, "System call error") != NULL);
    assert(strstr(buf, "No such file") != NULL);

    /* NULL / zero size buffer safety */
    assert(rbipc_error_format(RBIPC_ERR_SYS, 0, NULL, 0) == NULL);

    printf("[test_unit_error] All tests passed successfully.\n");
    return 0;
}
