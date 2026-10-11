/**
 * @file rbipc_error.c
 * @brief Implementation of error string translation for librbipc.
 * @details Implements a branch-table mapping of status codes to human-readable strings.
 */

#include "rbipc.h"
#include "rbipc_error.h"

/**
 * @brief Maps an error code to a static description string.
 */
RBIPC_RETURNS_NONNULL RBIPC_CONST RBIPC_COLD RBIPC_LEAF
const char *rbipc_strerror(int err) {
    switch (err) {
        case RBIPC_OK:
            return "Success";
        case RBIPC_ERR_INVAL:
            return "Invalid argument or configuration";
        case RBIPC_ERR_NOMEM:
            return "Memory allocation or mapping failure";
        case RBIPC_ERR_SYS:
            return "System call error (inspect errno)";
        case RBIPC_ERR_FULL:
            return "Ring buffer is currently full";
        case RBIPC_ERR_EMPTY:
            return "Ring buffer is currently empty";
        case RBIPC_ERR_POISONED:
            return "Slot producer crashed; slot poisoned";
        case RBIPC_ERR_SHUTDOWN:
            return "Ring buffer signaled shutdown";
        case RBIPC_ERR_TIMEOUT:
            return "Operation timed out";
        case RBIPC_ERR_BUSY:
            return "Resource is busy or in contention";
        case RBIPC_ERR_OVERFLOW:
            return "Arithmetic or buffer calculation overflow";
        case RBIPC_ERR_EXISTS:
            return "Shared memory object already exists";
        case RBIPC_ERR_NOTSUP:
            return "Operation not supported";
        default:
            return "Unknown error";
    }
}

#include <errno.h>
#include <stdio.h>
#include <string.h>

/**
 * @brief Translate a Linux system errno code to corresponding librbipc error code.
 *
 * @param[in] err System errno value.
 *
 * @return Mapped librbipc status code.
 */
RBIPC_PURE RBIPC_LEAF
int rbipc_errno_to_status(int err) {
    switch (err) {
        case 0:
            return RBIPC_OK;
        case EINVAL:
            return RBIPC_ERR_INVAL;
        case ENOMEM:
            return RBIPC_ERR_NOMEM;
        case EEXIST:
            return RBIPC_ERR_EXISTS;
        case ETIMEDOUT:
            return RBIPC_ERR_TIMEOUT;
        case EBUSY:
        case EAGAIN:
            return RBIPC_ERR_BUSY;
        case EOPNOTSUPP:
        case ENOSYS:
            return RBIPC_ERR_NOTSUP;
        default:
            return RBIPC_ERR_SYS;
    }
}

/**
 * @brief Format a composite diagnostic message combining status and system errno.
 *
 * @param[in]  status    librbipc status code.
 * @param[in]  sys_errno Linux system errno (or 0).
 * @param[out] buf       Destination character buffer.
 * @param[in]  buf_size  Capacity of destination buffer.
 *
 * @return Pointer to @p buf.
 */
RBIPC_LEAF
char *rbipc_error_format(int status, int sys_errno, char *buf, size_t buf_size) {
    if (RBIPC_UNLIKELY(!buf || buf_size == 0)) {
        return buf;
    }
    const char *status_str = rbipc_strerror(status);
    if (sys_errno != 0) {
        snprintf(buf, buf_size, "%s (system errno %d: %s)",
                 status_str, sys_errno, strerror(sys_errno));
    } else {
        snprintf(buf, buf_size, "%s", status_str);
    }
    return buf;
}
