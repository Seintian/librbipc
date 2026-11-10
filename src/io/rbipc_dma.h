/**
 * @file rbipc_dma.h
 * @brief Hardware-assisted asynchronous DMA offload engine interfaces (Intel
 * DSA).
 *
 * @details Implements memory operation offloading via on-chip Intel Data
 * Streaming Accelerator (DSA) using unprivileged user-mode work queues and
 * single-instruction descriptor dispatch (ENQCMD / MOVDIR64B). Bypasses CPU
 * load-store unit saturation for large payloads (>= 16 KiB), with zero-overhead
 * streaming store fallback.
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
 * @def RBIPC_DSA_DESC_SIZE
 * @brief Intel Data Streaming Accelerator (DSA) hardware descriptor size
 * specification in bytes.
 * @note Strictly 64 bytes per Intel DSA hardware specification (MOVDIR64B /
 * ENQCMD), decoupled from host CPU cache line size.
 */
#define RBIPC_DSA_DESC_SIZE 64U

/**
 * @def RBIPC_DMA_OFFLOAD_THRESHOLD
 * @brief Minimum payload byte size where hardware DMA offload outperforms CPU
 * vector registers.
 * @details Set to 16,384 bytes (16 KB), the break-even point where PCIe/MMIO
 * portal dispatch latency (~150-400 ns) is amortized against in-core
 * AVX-512/AVX2 memcpy.
 */
#define RBIPC_DMA_OFFLOAD_THRESHOLD 16384U

/**
 * @def RBIPC_DSA_DEV_DIR
 * @brief Directory where the accel-config driver exposes DSA work-queue nodes.
 */
#define RBIPC_DSA_DEV_DIR "/dev/dsa"

/**
 * @def RBIPC_DSA_WQ_PATH
 * @brief First DSA work-queue character device node.
 */
#define RBIPC_DSA_WQ_PATH "/dev/dsa/wq0.0"

/**
 * @def RBIPC_DSA_PORTAL_SIZE
 * @brief Size in bytes of the DSA work-queue MMIO submission portal (one page).
 */
#define RBIPC_DSA_PORTAL_SIZE 4096U

/**
 * @def RBIPC_DSA_OP_MEMMOVE
 * @brief DSA descriptor opcode for a memory move operation.
 */
#define RBIPC_DSA_OP_MEMMOVE 0x03U

/**
 * @def RBIPC_DSA_FLAG_RCR
 * @brief DSA descriptor flag requesting a completion record write
 * (IDXD_OP_FLAG_RCR).
 */
#define RBIPC_DSA_FLAG_RCR 0x0008U

/**
 * @def RBIPC_DMA_COMPLETION_STATUS_SUCCESS
 * @brief DSA completion record status value indicating a successful operation.
 */
#define RBIPC_DMA_COMPLETION_STATUS_SUCCESS 0x01U

/**
 * @def RBIPC_DMA_COMPLETION_STATUS_PAGE_FAULT
 * @brief DSA completion record status value indicating a page fault during
 * transfer.
 */
#define RBIPC_DMA_COMPLETION_STATUS_PAGE_FAULT 0x02U

/**
 * @def RBIPC_DMA_COMPLETION_STATUS_BATCH_PAGE_FAULT
 * @brief DSA completion record status value indicating a batch page fault.
 */
#define RBIPC_DMA_COMPLETION_STATUS_BATCH_PAGE_FAULT 0x03U

/**
 * @def RBIPC_DMA_COMPLETION_STATUS_INVALID
 * @brief DSA completion record status value indicating an invalid descriptor.
 */
#define RBIPC_DMA_COMPLETION_STATUS_INVALID 0x04U

/**
 * @def RBIPC_DMA_COMPLETION_STATUS_RSVD
 * @brief DSA completion record status value indicating a reserved/unsupported
 * operation.
 */
#define RBIPC_DMA_COMPLETION_STATUS_RSVD 0x05U

/**
 * @def RBIPC_DMA_COMPLETION_STATUS_CRC_ERR
 * @brief DSA completion record status value indicating a CRC error.
 */
#define RBIPC_DMA_COMPLETION_STATUS_CRC_ERR 0x06U

/**
 * @def RBIPC_DMA_COMPLETION_STATUS_CRC_ERR_RSVD
 * @brief DSA completion record status value indicating a CRC error on a
 * reserved field.
 */
#define RBIPC_DMA_COMPLETION_STATUS_CRC_ERR_RSVD 0x07U
/* NOTE: completion status values 0x08-0xFF are reserved by the Intel DSA spec
 * and are intentionally not enumerated here. */

/**
 * @struct rbipc_dma_desc
 * @brief Intel Data Streaming Accelerator (DSA) 64-byte hardware descriptor
 * layout.
 */
typedef struct rbipc_dma_desc {
    _Alignas(RBIPC_DSA_DESC_SIZE) uint32_t
        pasid_flags;          /**< Process Address Space ID and control flags */
    uint32_t flags;           /**< Operation flags (request completion, CRC) */
    uint64_t completion_addr; /**< User virtual address of completion record */
    uint64_t src_addr;        /**< Source virtual address */
    uint64_t dst_addr;        /**< Destination virtual address */
    uint32_t xfer_size;       /**< Byte count to transfer */
    uint16_t int_handle;      /**< User interrupt handle */
    uint8_t op;               /**< DSA opcode (0x03 = MEMMOVE) */
    uint8_t reserved[25];
} rbipc_dma_desc_t;

static_assert(sizeof(rbipc_dma_desc_t) == RBIPC_DSA_DESC_SIZE,
              "rbipc_dma_desc_t must be exactly 64 bytes for MOVDIR64B / "
              "ENQCMD per Intel DSA spec");
static_assert(alignof(rbipc_dma_desc_t) == RBIPC_DSA_DESC_SIZE,
              "rbipc_dma_desc_t must be 64-byte aligned per Intel DSA spec");

/**
 * @struct rbipc_dma_completion
 * @brief Intel DSA completion record (32 bytes) written by the hardware when an
 *        operation completes. The first byte holds the status; the hardware
 *        writes 0x01 (success) or an error code on completion.
 */
typedef struct rbipc_dma_completion {
    uint8_t status;      /**< Completion status (0x01 = success, else error) */
    uint8_t reserved[7]; /**< Reserved */
    uint64_t bytes_completed; /**< Bytes successfully transferred */
    uint64_t reserved2;       /**< Reserved */
    uint64_t reserved3;       /**< Reserved */
} rbipc_dma_completion_t;

static_assert(
    sizeof(rbipc_dma_completion_t) == 32U,
    "rbipc_dma_completion_t must be exactly 32 bytes per Intel DSA spec");

/**
 * @brief Dispatch a 64-byte DSA descriptor to a work queue portal via
 * MOVDIR64B.
 * @details Executes the MOVDIR64B instruction, which writes the descriptor to
 * the WQ's MMIO portal. Returns 0 on successful submission (descriptor
 * accepted) or non-zero if the WQ is full.
 * @param[in] portal  MMIO portal address of the shared work queue.
 * @param[in] desc    Pointer to the 64-byte aligned descriptor.
 * @return 0 on successful submission, non-zero if the WQ is full.
 */
RBIPC_INLINE int rbipc_dma_movdir64b(
    void *RBIPC_RESTRICT portal, const rbipc_dma_desc_t *RBIPC_RESTRICT desc) {
#if defined(__x86_64__)
    int rc;
    __asm__ volatile("movdir64b (%1), %0\n\t"
                     "setz %b2\n\t"
                     : "+r"(portal), "+r"(desc), "=r"(rc)
                     :
                     : "memory");
    /* MOVDIR64B sets ZF=1 when the descriptor is accepted, ZF=0 when the WQ is
     * full. setz captures that flag into the low byte of rc. */
    return rc ? 0 : -1;
#else
    (void)portal;
    (void)desc;
    return -1;
#endif
}

#ifdef __cplusplus
}
#endif

#endif /* RBIPC_DMA_H */
