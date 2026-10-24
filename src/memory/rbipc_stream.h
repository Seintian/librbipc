/**
 * @file rbipc_stream.h
 * @brief Non-temporal cache-bypassing memory streaming engine.
 *
 * @details Implements hardware-accelerated memory streaming pipelines using
 * AVX2
 * (_mm256_stream_si256) and SSE2 (_mm_stream_si128) non-temporal stores to
 * bypass the CPU cache hierarchy during large payload transfers, minimizing
 * cache pollution.
 *
 * References:
 * - Ulrich Drepper (2007), "What Every Programmer Should Know About Memory".
 * - Intel 64 and IA-32 Architectures Optimization Reference Manual (Streaming
 * Stores).
 *
 * @author Seintian
 * @date 2026
 * @copyright Apache License 2.0
 */

#ifndef RBIPC_STREAM_H
#define RBIPC_STREAM_H

#include "rbipc.h"
#include "rbipc_attr.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/**
 * @def RBIPC_STREAM_THRESHOLD_DEFAULT
 * @brief Conservative default streaming threshold (256 KB) when dynamic cache discovery is unavailable.
 */
#define RBIPC_STREAM_THRESHOLD_DEFAULT (256ULL * 1024ULL)

#define RBIPC_STREAM_THRESHOLD RBIPC_STREAM_THRESHOLD_DEFAULT

/**
 * @brief Retrieve dynamic optimal non-temporal streaming store threshold.
 * @details Evaluates L3/L2 cache size dynamically (defaulting to 3/4 L3 or 512 KB, minimum 256 KB).
 */
RBIPC_NODISCARD RBIPC_LEAF size_t rbipc_stream_threshold(void);

/**
 * @brief Copy memory buffer using cache-bypassing non-temporal streaming
 * stores.
 *
 * @param[out] dst  Destination memory buffer in shared memory.
 * @param[in]  src  Source memory buffer.
 * @param[in]  size Number of bytes to transfer.
 *
 * @return RBIPC_OK on success, or RBIPC_ERR_INVAL if pointers are null.
 */
RBIPC_NODISCARD RBIPC_LEAF int rbipc_stream_copy(void *RBIPC_RESTRICT dst,
                                                 const void *RBIPC_RESTRICT src,
                                                 size_t size);

/**
 * @brief Automatically select streaming copy or standard memcpy based on dynamic cache threshold.
 *
 * @param[out] dst  Destination memory buffer.
 * @param[in]  src  Source memory buffer.
 * @param[in]  size Number of bytes to transfer.
 */
RBIPC_INLINE void rbipc_stream_copy_auto(void *RBIPC_RESTRICT dst,
                                         const void *RBIPC_RESTRICT src,
                                         size_t size) {
  if (size >= rbipc_stream_threshold()) {
    int rc = rbipc_stream_copy(dst, src, size);
    (void)rc;
  } else {
    memcpy(dst, src, size);
  }
}

/**
 * @brief Transmit payload via non-temporal streaming stores with a timeout
 * deadline.
 *
 * @param[in,out] ring       Ring buffer handle.
 * @param[in]     src        Source payload buffer.
 * @param[in]     len        Payload length in bytes.
 * @param[in]     timeout_ns Timeout deadline in nanoseconds (0 = nonblock,
 * UINT64_MAX = unbounded).
 * @param[out]    out_ticket Location to store the assigned sequence ticket.
 *
 * @return RBIPC_OK on success, or appropriate error code.
 */
RBIPC_NODISCARD RBIPC_LEAF int rbipc_write_stream_timeout(
    rbipc_ring_t *RBIPC_RESTRICT ring, const void *RBIPC_RESTRICT src,
    uint32_t len, uint64_t timeout_ns, uint64_t *RBIPC_RESTRICT out_ticket);

/**
 * @brief Transmit payload via non-temporal streaming stores (Blocking).
 *
 * @param[in,out] ring       Ring buffer handle.
 * @param[in]     src        Source payload buffer.
 * @param[in]     len        Payload length in bytes.
 * @param[out]    out_ticket Location to store the assigned sequence ticket.
 *
 * @return RBIPC_OK on success, or appropriate error code.
 */
RBIPC_NODISCARD RBIPC_LEAF int
rbipc_write_stream(rbipc_ring_t *RBIPC_RESTRICT ring,
                   const void *RBIPC_RESTRICT src, uint32_t len,
                   uint64_t *RBIPC_RESTRICT out_ticket);

/**
 * @brief Transmit payload via non-temporal streaming stores (Non-blocking).
 *
 * @param[in,out] ring       Ring buffer handle.
 * @param[in]     src        Source payload buffer.
 * @param[in]     len        Payload length in bytes.
 * @param[out]    out_ticket Location to store the assigned sequence ticket.
 *
 * @return RBIPC_OK on success, or appropriate error code.
 */
RBIPC_NODISCARD RBIPC_LEAF int
rbipc_write_stream_nonblock(rbipc_ring_t *RBIPC_RESTRICT ring,
                            const void *RBIPC_RESTRICT src, uint32_t len,
                            uint64_t *RBIPC_RESTRICT out_ticket);

#endif /* RBIPC_STREAM_H */
