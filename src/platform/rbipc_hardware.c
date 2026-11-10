/**
 * @file rbipc_hardware.c
 * @brief Implementation of dynamic hardware topology discovery and calibration.
 *
 * @author Seintian
 * @date 2026
 * @copyright Apache License 2.0
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "rbipc_hardware.h"
#include "rbipc_arch.h"
#include "rbipc_dma.h"
#include "rbipc_sync.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <unistd.h>
#include <time.h>

#if defined(__linux__)
#include <sys/auxv.h>
#endif

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) ||             \
    defined(_M_IX86)
#include <cpuid.h>
#endif

static rbipc_hardware_profile_t s_profile;
static _Atomic int s_profile_state =
    0; /* 0 = uninit, 1 = initializing, 2 = ready */

static uint64_t rbipc_detect_tsc_ticks_per_ns(void) {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) ||             \
    defined(_M_IX86)
    /* Method 1: CPUID Leaf 0x15 (Crystal Clock Frequency & Ratio) */
    unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
    if (__get_cpuid(0x15, &eax, &ebx, &ecx, &edx) && eax > 0 && ebx > 0) {
        uint64_t crystal = ecx;
        if (crystal == 0) {
            /* Standard nominal crystal: 24 MHz on client Core, 25 MHz on server
             * Skylake-X */
            crystal = RBIPC_TSC_CRYSTAL_24MHZ;
        }
        uint64_t tsc_hz = (crystal * (uint64_t)ebx) / (uint64_t)eax;
        if (tsc_hz > 0) {
            return (tsc_hz + (RBIPC_NSEC_PER_SEC / 2)) / RBIPC_NSEC_PER_SEC;
        }
    }

    /* Method 2: CPUID Leaf 0x16 (Processor Base Frequency in MHz) */
    if (__get_cpuid(0x16, &eax, &ebx, &ecx, &edx) && eax > 0) {
        uint64_t base_mhz = eax;
        return (base_mhz + 500ULL) / 1000ULL;
    }

    /* Method 3: Linux sysfs tsc_freq_khz */
    FILE *f = fopen(RBIPC_TSC_FREQ_SYSFS, "r");
    if (f) {
        char buf[64];
        if (fgets(buf, sizeof(buf), f)) {
            char *endptr = NULL;
            uint64_t khz = strtoull(buf, &endptr, 10);
            fclose(f);
            if (khz > 0) {
                uint64_t computed = (khz + 500000ULL) / 1000000ULL;
                if (computed > 0)
                    return computed;
            }
        } else {
            fclose(f);
        }
    }

    /* Method 4: Calibrate against CLOCK_MONOTONIC with memory barriers */
    struct timespec ts1, ts2;
    clock_gettime(CLOCK_MONOTONIC, &ts1);
    __asm__ __volatile__("lfence" ::: "memory");
    uint64_t tsc1 = rbipc_rdtsc();
    __asm__ __volatile__("lfence" ::: "memory");

    for (volatile int i = 0; i < RBIPC_TSC_CALIBRATION_LOOPS; i++) {
        rbipc_cpu_pause();
    }

    __asm__ __volatile__("lfence" ::: "memory");
    uint64_t tsc2 = rbipc_rdtsc();
    __asm__ __volatile__("lfence" ::: "memory");
    clock_gettime(CLOCK_MONOTONIC, &ts2);

    uint64_t elapsed_ns =
        (uint64_t)(ts2.tv_sec - ts1.tv_sec) * RBIPC_NSEC_PER_SEC +
        (uint64_t)(ts2.tv_nsec - ts1.tv_nsec);
    if (elapsed_ns > 0 && tsc2 > tsc1) {
        uint64_t computed = (tsc2 - tsc1) / elapsed_ns;
        if (computed > 0)
            return computed;
    }

    /* Fallback: 3 GHz */
    return RBIPC_TSC_FALLBACK_GHZ;
#elif defined(__aarch64__)
    uint64_t freq = rbipc_arm_cntfrq();
    if (freq > 0) {
        return (freq + (RBIPC_NSEC_PER_SEC / 2)) / RBIPC_NSEC_PER_SEC;
    }
    return 1;
#else
    return 1;
#endif
}

/**
 * @brief Detect the microarchitectural spin budget (PAUSE iterations).
 * @details On Skylake/Ice Lake where PAUSE takes ~140 cycles, 16 pauses is
 *          ~2240 cycles (~700 ns); other x86 and non-x86 use a 48-iteration
 *          default.
 */
static uint32_t rbipc_detect_spin_count(void) {
    uint32_t spin = RBIPC_SPIN_COUNT_DEFAULT;
#if defined(__x86_64__) || defined(_M_X64)
    unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
    unsigned int family = 0, model = 0;
    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
        family = ((eax >> 8U) & 0x0FU) + ((eax >> 20U) & 0xFFU);
        model = ((eax >> 4U) & 0x0FU) | ((eax >> 12U) & 0xF0U);
        if (family == 6 && (model == RBIPC_CPU_MODEL_SKYLAKE_MOBILE ||
                            model == RBIPC_CPU_MODEL_SKYLAKE_DESKTOP ||
                            model == RBIPC_CPU_MODEL_KABYLAKE_MOBILE ||
                            model == RBIPC_CPU_MODEL_KABYLAKE_DESKTOP ||
                            model == RBIPC_CPU_MODEL_SKYLAKE_X)) {
            spin = RBIPC_SPIN_COUNT_SKYLAKE;
        }
    }
#endif
    return spin;
}

/**
 * @brief Compute the optimal non-temporal streaming threshold from cache sizes.
 * @details Uses 3/4 of L3 when present, else L2, with a 256 KB floor to avoid
 *          L1/L2 thrashing.
 */
static size_t rbipc_detect_stream_threshold(size_t l2, size_t l3) {
    size_t st = 0;
    if (l3 > 0) {
        st = (l3 * RBIPC_L3_FRACTION_NUM) / RBIPC_L3_FRACTION_DEN;
    } else if (l2 > 0) {
        st = l2;
    }
    if (st < RBIPC_STREAM_THRESHOLD_FLOOR) {
        st = RBIPC_STREAM_THRESHOLD_FLOOR;
    }
    return st;
}

/**
 * @brief Parse a base-10 unsigned environment override into @p out.
 * @details Returns true only when the variable is present, non-empty, and the
 *          entire string parses as a valid uint64 (rejecting trailing garbage
 *          and overflow via errno==ERANGE). Absent, empty, or malformed values
 *          leave @p out untouched and return false.
 */
static bool rbipc_env_u64(const char *name, uint64_t *out) {
    const char *val = getenv(name);
    if (!val || val[0] == '\0') {
        return false;
    }
    errno = 0;
    char *end = NULL;
    unsigned long long parsed = strtoull(val, &end, 10);
    if (errno == ERANGE || end == val || *end != '\0') {
        return false;
    }
    *out = (uint64_t)parsed;
    return true;
}

/**
 * @brief Apply optional RBIPC_SIMULATE_* environment overrides for
 * benchmarking.
 */
static void rbipc_profile_apply_env_overrides(void) {
    uint64_t v = 0;
    if (rbipc_env_u64("RBIPC_SIMULATE_PAGE_SIZE", &v)) {
        s_profile.page_size = (size_t)v;
    }
    if (rbipc_env_u64("RBIPC_SIMULATE_HUGE_PAGE_SIZE", &v)) {
        s_profile.huge_page_size = (size_t)v;
    }
    if (rbipc_env_u64("RBIPC_SIMULATE_L3_CACHE_SIZE", &v)) {
        s_profile.l3_cache_size = (size_t)v;
        s_profile.stream_threshold = (s_profile.l3_cache_size * 3) / 4;
    }
    if (rbipc_env_u64("RBIPC_SIMULATE_ONLINE_CORES", &v)) {
        s_profile.online_cores = (uint32_t)v;
    }
    if (rbipc_env_u64("RBIPC_SIMULATE_SPIN_COUNT", &v)) {
        s_profile.calibrated_spin_count = (uint32_t)v;
    }
    if (rbipc_env_u64("RBIPC_SIMULATE_TSC_FREQ", &v)) {
        s_profile.tsc_ticks_per_ns = v;
    }
}

static void rbipc_hardware_profile_init(void) {
    /* Reuse the cached arch.h accessors instead of re-probing the kernel. */
    s_profile.page_size = rbipc_page_size();
    s_profile.cache_line_size = rbipc_cache_line_size();
    s_profile.destructive_size = (s_profile.cache_line_size < RBIPC_CACHE_LINE)
                                     ? RBIPC_CACHE_LINE
                                     : s_profile.cache_line_size;
    s_profile.huge_page_size = rbipc_huge_page_size();

    /* Cache hierarchy (L2 and L3) via centralized cached accessors. */
    s_profile.l2_cache_size = rbipc_l2_cache_size();
    s_profile.l3_cache_size = rbipc_l3_cache_size();

    /* Optimal non-temporal streaming threshold. */
    s_profile.stream_threshold = rbipc_detect_stream_threshold(
        s_profile.l2_cache_size, s_profile.l3_cache_size);

    /* Intel DSA DMA offload threshold (16 KB break-even). */
    s_profile.dma_offload_threshold = RBIPC_DMA_OFFLOAD_THRESHOLD;

    /* Online processors via centralized cached accessor. */
    s_profile.online_cores = rbipc_online_cores();

    /* Microarchitectural spin budget. */
    s_profile.calibrated_spin_count = rbipc_detect_spin_count();

    /* TSC / timer ticks per nanosecond. */
    s_profile.tsc_ticks_per_ns = rbipc_detect_tsc_ticks_per_ns();

    /* Optional test / benchmark simulation overrides. */
    rbipc_profile_apply_env_overrides();
}

RBIPC_LEAF
const rbipc_hardware_profile_t *rbipc_get_hardware_profile(void) {
    int state = atomic_load_explicit(&s_profile_state, memory_order_acquire);
    if (RBIPC_LIKELY(state == 2)) {
        return &s_profile;
    }

    int expected = 0;
    if (atomic_compare_exchange_strong_explicit(&s_profile_state, &expected, 1,
                                                memory_order_acq_rel,
                                                memory_order_acquire)) {
        rbipc_hardware_profile_init();
        atomic_store_explicit(&s_profile_state, 2, memory_order_release);
    } else {
        while (atomic_load_explicit(&s_profile_state, memory_order_acquire) !=
               2) {
            rbipc_cpu_pause();
        }
    }

    return &s_profile;
}

/* Generates a leaf accessor that returns a single field of the cached
 * hardware profile. All accessors share the identical shape; the macro
 * parameterizes only the function name, return type, and profile field. */
#define RBIPC_HW_ACCESSOR(name, type, field)                                   \
    RBIPC_LEAF type name(void) {                                               \
        return rbipc_get_hardware_profile()->field;                            \
    }

RBIPC_HW_ACCESSOR(rbipc_hardware_cache_line_size, size_t, cache_line_size)
RBIPC_HW_ACCESSOR(rbipc_hardware_page_size, size_t, page_size)
RBIPC_HW_ACCESSOR(rbipc_hardware_huge_page_size, size_t, huge_page_size)
RBIPC_HW_ACCESSOR(rbipc_hardware_streaming_threshold, size_t, stream_threshold)
RBIPC_HW_ACCESSOR(rbipc_hardware_dma_threshold, size_t, dma_offload_threshold)
RBIPC_HW_ACCESSOR(rbipc_hardware_online_cores, uint32_t, online_cores)
RBIPC_HW_ACCESSOR(rbipc_hardware_tsc_ticks_per_ns, uint64_t, tsc_ticks_per_ns)

#undef RBIPC_HW_ACCESSOR
