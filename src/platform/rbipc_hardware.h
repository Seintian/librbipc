/**
 * @file rbipc_hardware.h
 * @brief Dynamic hardware topology discovery, page geometry, and microarchitectural calibration.
 *
 * @details Discovers CPU cache hierarchy, page sizes, HugeTLB configurations,
 *          and microarchitectural timing parameters dynamically at runtime with zero-overhead
 *          atomic caching on subsequent accesses.
 *
 * @author Seintian
 * @date 2026
 * @copyright Apache License 2.0
 */

#ifndef RBIPC_HARDWARE_H
#define RBIPC_HARDWARE_H

#include "rbipc.h"
#include "rbipc_attr.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize and retrieve process-cached hardware profile.
 * Zero-overhead atomic acquire on fast path.
 */
RBIPC_API RBIPC_NODISCARD const rbipc_hardware_profile_t *
rbipc_get_hardware_profile(void);

/**
 * @brief Query dynamic L1 data cache line size in bytes.
 */
RBIPC_INTERNAL RBIPC_NODISCARD size_t rbipc_hardware_cache_line_size(void);

/**
 * @brief Query dynamic virtual memory base page size in bytes.
 */
RBIPC_INTERNAL RBIPC_NODISCARD size_t rbipc_hardware_page_size(void);

/**
 * @brief Query dynamic system default HugeTLB page size in bytes.
 */
RBIPC_INTERNAL RBIPC_NODISCARD size_t rbipc_hardware_huge_page_size(void);

/**
 * @brief Query dynamic non-temporal streaming cutoff threshold in bytes.
 */
RBIPC_INTERNAL RBIPC_NODISCARD size_t rbipc_hardware_streaming_threshold(void);

/**
 * @brief Query dynamic Intel DSA offload cutoff threshold in bytes.
 */
RBIPC_INTERNAL RBIPC_NODISCARD size_t rbipc_hardware_dma_threshold(void);

/**
 * @brief Query dynamic online core count.
 */
RBIPC_INTERNAL RBIPC_NODISCARD uint32_t rbipc_hardware_online_cores(void);

/**
 * @brief Query calibrated or crystal-reported TSC ticks per nanosecond.
 */
RBIPC_INTERNAL RBIPC_NODISCARD uint64_t rbipc_hardware_tsc_ticks_per_ns(void);

#ifdef __cplusplus
}
#endif

#endif /* RBIPC_HARDWARE_H */
