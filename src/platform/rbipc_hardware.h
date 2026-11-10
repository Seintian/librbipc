/**
 * @file rbipc_hardware.h
 * @brief Dynamic hardware topology discovery, page geometry, and
 * microarchitectural calibration.
 *
 * @details Discovers CPU cache hierarchy, page sizes, HugeTLB configurations,
 *          and microarchitectural timing parameters dynamically at runtime with
 * zero-overhead atomic caching on subsequent accesses.
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
 * @def RBIPC_TSC_CRYSTAL_24MHZ
 * @brief Nominal TSC crystal frequency (Hz) on client Intel Core processors.
 */
#define RBIPC_TSC_CRYSTAL_24MHZ 24000000ULL

/**
 * @def RBIPC_TSC_FALLBACK_GHZ
 * @brief Fallback TSC frequency (GHz) when no detection method succeeds.
 */
#define RBIPC_TSC_FALLBACK_GHZ 3U

/**
 * @def RBIPC_TSC_CALIBRATION_LOOPS
 * @brief PAUSE iterations used to calibrate TSC frequency against
 * CLOCK_MONOTONIC.
 */
#define RBIPC_TSC_CALIBRATION_LOOPS 2000

/**
 * @def RBIPC_TSC_FREQ_SYSFS
 * @brief sysfs file exposing the TSC frequency in kHz.
 */
#define RBIPC_TSC_FREQ_SYSFS "/sys/devices/system/cpu/cpu0/tsc_freq_khz"

/**
 * @def RBIPC_SPIN_COUNT_SKYLAKE
 * @brief PAUSE spin budget for Skylake/Ice Lake microarchitectures.
 */
#define RBIPC_SPIN_COUNT_SKYLAKE 16U

/**
 * @def RBIPC_CPU_MODEL_SKYLAKE_MOBILE
 * @brief CPUID model ID for Skylake mobile (family 6).
 */
#define RBIPC_CPU_MODEL_SKYLAKE_MOBILE 0x4EU

/**
 * @def RBIPC_CPU_MODEL_SKYLAKE_DESKTOP
 * @brief CPUID model ID for Skylake desktop (family 6).
 */
#define RBIPC_CPU_MODEL_SKYLAKE_DESKTOP 0x5EU

/**
 * @def RBIPC_CPU_MODEL_KABYLAKE_MOBILE
 * @brief CPUID model ID for Kaby Lake mobile (family 6).
 */
#define RBIPC_CPU_MODEL_KABYLAKE_MOBILE 0x8EU

/**
 * @def RBIPC_CPU_MODEL_KABYLAKE_DESKTOP
 * @brief CPUID model ID for Kaby Lake desktop (family 6).
 */
#define RBIPC_CPU_MODEL_KABYLAKE_DESKTOP 0x9EU

/**
 * @def RBIPC_CPU_MODEL_SKYLAKE_X
 * @brief CPUID model ID for Skylake-X (family 6).
 */
#define RBIPC_CPU_MODEL_SKYLAKE_X 0x55U

/**
 * @def RBIPC_STREAM_THRESHOLD_FLOOR
 * @brief Minimum non-temporal streaming threshold (bytes) to avoid L1/L2
 * thrashing.
 */
#define RBIPC_STREAM_THRESHOLD_FLOOR (256ULL * 1024ULL)

/**
 * @def RBIPC_L3_FRACTION_NUM
 * @brief Numerator of the L3 fraction used for the streaming threshold.
 */
#define RBIPC_L3_FRACTION_NUM 3U

/**
 * @def RBIPC_L3_FRACTION_DEN
 * @brief Denominator of the L3 fraction used for the streaming threshold.
 */
#define RBIPC_L3_FRACTION_DEN 4U

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
