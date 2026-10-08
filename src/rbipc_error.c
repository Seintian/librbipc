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
        default:
            return "Unknown error";
    }
}
