/**
 * @file rbipc_error.c
 * @brief Implementation of error string translation for librbipc.
 * @details Implements a branch-table mapping of status codes to human-readable
 * strings.
 */

#include "rbipc.h"
#include "rbipc_error.h"

#include <errno.h>
#include <string.h>

/**
 * @brief Maps an error code to a static description string.
 */
RBIPC_RETURNS_NONNULL RBIPC_CONST RBIPC_COLD RBIPC_LEAF const char *
rbipc_strerror(int err) {
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

#include <stdio.h>

/**
 * @brief Helper: thread-safely translate an errno value into a caller buffer.
 * @details Wraps strerror_r() so both the GNU and XSI/portable signatures are
 * supported (selected by feature-test macros). Never aliases the format
 * buffer, so it is safe to use the result as part of a later snprintf().
 */
static void rbipc_strerror_sys(int sys_errno, char *out, size_t out_size) {
    if (RBIPC_UNLIKELY(!out || out_size == 0)) {
        return;
    }
#ifdef __USE_GNU
    char *msg = strerror_r(sys_errno, out, out_size);
    if (msg != NULL && msg != out) {
        /*
         * GNU strerror_r may return a pointer to an internal static buffer
         * instead of the caller-provided one. Copy the result out so the
         * caller's buffer is always populated uniformly.
         */
        size_t n = strnlen(msg, out_size - 1);
        memcpy(out, msg, n);
        out[n] = '\0';
    }
#else
    const int saved = errno;
    if (strerror_r(sys_errno, out, out_size) != 0) {
        int n = snprintf(out, out_size, "Unknown error %d", sys_errno);
        (void)n;
    } else if (out[0] == '\0') {
        /* Empty string is not useful; substitute a minimal fallback. */
        int n = snprintf(out, out_size, "Unknown error %d", sys_errno);
        (void)n;
    }
    errno = saved;
#endif
}

/**
 * @brief Translate a Linux system errno code to corresponding librbipc error
 * code.
 */
RBIPC_PURE RBIPC_LEAF int rbipc_errno_to_status(int err) {
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
 * @brief Format a composite diagnostic message combining status and system
 * errno.
 *
 * @details Uses strerror_r() (the thread-safe per-thread buffer variant)
 * instead of strerror() so concurrent callers from multiple threads/processes
 * cannot corrupt a shared static buffer.
 */
RBIPC_LEAF
char *rbipc_error_format(int status, int sys_errno, char *buf,
                         size_t buf_size) {
    if (RBIPC_UNLIKELY(!buf || buf_size == 0)) {
        return buf;
    }
    const char *status_str = rbipc_strerror(status);
    if (sys_errno != 0) {
        /*
         * Populate a dedicated scratch buffer via strerror_r() (thread-safe,
         * per-thread storage) and then compose into the caller's buffer. Using
         * a separate scratch avoids output/input aliasing: strerror_r() and
         * snprintf() never read and write the same bytes at the same time.
         */
        char sys_str[128];
        rbipc_strerror_sys(sys_errno, sys_str, sizeof(sys_str));
        snprintf(buf, buf_size, "%s (system errno %d: %s)", status_str,
                 sys_errno, sys_str);
    } else {
        snprintf(buf, buf_size, "%s", status_str);
    }
    return buf;
}
