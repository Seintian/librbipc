/**
 * @file rbipc_error.h
 * @brief Error code definitions and string formatting helpers
 */

#ifndef RBIPC_ERROR_H
#define RBIPC_ERROR_H

#include "rbipc.h"
#include "rbipc_attr.h"

/**
 * @brief Convert an error code into a human-readable description string.
 *
 * @param err Return code from any rbipc function.
 * @return Static string describing the error.
 */
RBIPC_RETURNS_NONNULL RBIPC_CONST RBIPC_COLD
const char *rbipc_strerror(int err);

#endif /* RBIPC_ERROR_H */
