/**
 * @file rbipc_error.h
 * @brief Status code description formatting and error reporting utilities.
 *
 * @details Declares string formatting functions for mapping negative @ref rbipc_errors
 * status codes into human-readable diagnostics.
 *
 * @author Christian Santarelli
 * @date 2026
 * @copyright Apache License 2.0
 */

#ifndef RBIPC_ERROR_H
#define RBIPC_ERROR_H

#include "rbipc.h"
#include "rbipc_attr.h"

/**
 * @brief Convert an integer status code into a constant human-readable description string.
 *
 * @param[in] err Return code emitted by any librbipc API function.
 *
 * @return Non-null pointer to a static, read-only diagnostic string literal.
 */
RBIPC_RETURNS_NONNULL RBIPC_CONST RBIPC_COLD RBIPC_LEAF
const char *rbipc_strerror(int err);

#endif /* RBIPC_ERROR_H */
