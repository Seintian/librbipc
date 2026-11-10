/**
 * @file rbipc_uring.h
 * @brief Unified Linux io_uring asynchronous kernel-bypass zero-copy interfaces.
 *
 * @details Implements direct fixed-buffer registration (IORING_REGISTER_BUFFERS)
 *          and submission queue polling (IORING_SETUP_SQPOLL) between librbipc shared
 *          memory rings and the Linux kernel, enabling zero-copy network/disk ingestion.
 *
 * @author Seintian
 * @date 2026
 * @copyright Apache License 2.0
 */

#ifndef RBIPC_URING_H
#define RBIPC_URING_H

#include "rbipc.h"
#include "rbipc_attr.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @def RBIPC_URING_DEFAULT_ENTRIES
 * @brief Default submission queue depth used when probing io_uring availability.
 */
#define RBIPC_URING_DEFAULT_ENTRIES 2U

#ifdef __cplusplus
}
#endif

#endif /* RBIPC_URING_H */
