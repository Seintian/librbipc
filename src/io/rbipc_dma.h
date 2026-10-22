/**
 * @file rbipc_dma.h
 * @brief Hardware-assisted asynchronous DMA offload engine interfaces (Intel DSA).
 *
 * @details Implements memory operation offloading via on-chip Intel Data Streaming
 *          Accelerator (DSA) using unprivileged user-mode work queues and single-instruction
 *          descriptor dispatch (ENQCMD / MOVDIR64B). Bypasses CPU load-store unit saturation
 *          for large payloads (>= 4 KiB), with zero-overhead streaming store fallback.
 *
 * @author Seintian
 * @date 2026
 * @copyright Apache License 2.0
 */

#ifndef RBIPC_DMA_H
#define RBIPC_DMA_H

#include "rbipc.h"
#include "rbipc_attr.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @def RBIPC_DMA_OFFLOAD_THRESHOLD
 * @brief Minimum payload byte size where hardware DMA offload outperforms CPU vector registers.
 */
#define RBIPC_DMA_OFFLOAD_THRESHOLD 4096U

/**
 * @struct rbipc_dma_desc
 * @brief Intel Data Streaming Accelerator (DSA) 64-byte hardware descriptor layout.
 */
typedef struct rbipc_dma_desc {
  _Alignas(64) uint32_t pasid_flags;           /**< Process Address Space ID and control flags */
  uint32_t flags;                 /**< Operation flags (request completion, CRC) */
  uint64_t completion_addr;       /**< User virtual address of completion record */
  uint64_t src_addr;              /**< Source virtual address */
  uint64_t dst_addr;              /**< Destination virtual address */
  uint32_t xfer_size;             /**< Byte count to transfer */
  uint16_t int_handle;            /**< User interrupt handle */
  uint8_t  op;                    /**< DSA opcode (0x03 = MEMMOVE) */
  uint8_t  reserved[25];
} rbipc_dma_desc_t;

static_assert(sizeof(rbipc_dma_desc_t) == 64,
              "rbipc_dma_desc_t must be exactly 64 bytes for MOVDIR64B / ENQCMD");
static_assert(alignof(rbipc_dma_desc_t) == 64,
              "rbipc_dma_desc_t must be 64-byte aligned");

#ifdef __cplusplus
}
#endif

#endif /* RBIPC_DMA_H */
