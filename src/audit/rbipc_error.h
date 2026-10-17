/**
 * @file rbipc_error.h
 * @brief Status code description formatting and internal error diagnostics.
 * @details Declares diagnostic formatting and translation helpers for mapping
 *          status codes and system errno values to human-readable strings.
 */

#ifndef RBIPC_ERROR_H
#define RBIPC_ERROR_H

#include "rbipc.h"
#include "rbipc_attr.h"
#include <stddef.h>
#include <stdbool.h>

/**
 * @brief Check if a return status represents an error.
 */
RBIPC_INLINE RBIPC_CONST bool rbipc_status_is_error(int status) {
    return status < 0;
}

/**
 * @brief Map standard Linux system errno to a corresponding rbipc error code.
 *
 * @param[in] err System errno code.
 * @return Mapped RBIPC error code.
 */
RBIPC_INTERNAL RBIPC_PURE RBIPC_LEAF
int rbipc_errno_to_status(int err);

/**
 * @brief Formats a composite error diagnostic combining rbipc error and system errno.
 *
 * @param[in]  status    RBIPC status code.
 * @param[in]  sys_errno System errno value (or 0 if none).
 * @param[out] buf       Destination buffer for the diagnostic string.
 * @param[in]  buf_size  Capacity of destination buffer in bytes.
 *
 * @return Pointer to @p buf.
 */
RBIPC_INTERNAL RBIPC_LEAF
char *rbipc_error_format(int status, int sys_errno, char *buf, size_t buf_size);

#endif /* RBIPC_ERROR_H */
