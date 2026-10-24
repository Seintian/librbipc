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

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <cpuid.h>
#endif

static rbipc_hardware_profile_t s_profile;
static _Atomic int s_profile_state = 0; /* 0 = uninit, 1 = initializing, 2 = ready */

static uint64_t rbipc_detect_tsc_ticks_per_ns(void) {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
  /* Method 1: CPUID Leaf 0x15 (Crystal Clock Frequency & Ratio) */
  unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
  if (__get_cpuid(0x15, &eax, &ebx, &ecx, &edx) && eax > 0 && ebx > 0) {
    uint64_t crystal = ecx;
    if (crystal == 0) {
      /* Standard nominal crystal: 24 MHz on client Core, 25 MHz on server Skylake-X */
      crystal = 24000000ULL;
    }
    uint64_t tsc_hz = (crystal * (uint64_t)ebx) / (uint64_t)eax;
    if (tsc_hz > 0) {
      return (tsc_hz + 500000000ULL) / 1000000000ULL;
    }
  }

  /* Method 2: CPUID Leaf 0x16 (Processor Base Frequency in MHz) */
  if (__get_cpuid(0x16, &eax, &ebx, &ecx, &edx) && eax > 0) {
    uint64_t base_mhz = eax;
    return (base_mhz + 500ULL) / 1000ULL;
  }

  /* Method 3: Linux sysfs tsc_freq_khz */
  FILE *f = fopen("/sys/devices/system/cpu/cpu0/tsc_freq_khz", "r");
  if (f) {
    char buf[64];
    if (fgets(buf, sizeof(buf), f)) {
      char *endptr = NULL;
      uint64_t khz = strtoull(buf, &endptr, 10);
      fclose(f);
      if (khz > 0) {
        uint64_t computed = (khz + 500000ULL) / 1000000ULL;
        if (computed > 0) return computed;
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

  for (volatile int i = 0; i < 2000; i++) {
    rbipc_cpu_pause();
  }

  __asm__ __volatile__("lfence" ::: "memory");
  uint64_t tsc2 = rbipc_rdtsc();
  __asm__ __volatile__("lfence" ::: "memory");
  clock_gettime(CLOCK_MONOTONIC, &ts2);

  uint64_t elapsed_ns =
      (uint64_t)(ts2.tv_sec - ts1.tv_sec) * 1000000000ULL +
      (uint64_t)(ts2.tv_nsec - ts1.tv_nsec);
  if (elapsed_ns > 0 && tsc2 > tsc1) {
    uint64_t computed = (tsc2 - tsc1) / elapsed_ns;
    if (computed > 0) return computed;
  }

  /* Fallback: 3 GHz */
  return 3;
#elif defined(__aarch64__)
  uint64_t freq = rbipc_arm_cntfrq();
  if (freq > 0) {
    return (freq + 500000000ULL) / 1000000000ULL;
  }
  return 1;
#else
  return 1;
#endif
}

static void rbipc_hardware_profile_init(void) {
  /* 1. Base Virtual Memory Page Size */
  size_t ps = 0;
#if defined(__linux__)
  unsigned long aux_ps = getauxval(AT_PAGESZ);
  if (aux_ps > 0) ps = (size_t)aux_ps;
#endif
  if (ps == 0) {
    long raw = sysconf(_SC_PAGESIZE);
    ps = (raw > 0) ? (size_t)raw : 4096U;
  }
  s_profile.page_size = ps;

  /* 2. Physical Cache Line Size & Destructive Size */
  size_t cls = 0;
#if defined(__linux__) && defined(AT_DCACHEBSIZE)
  unsigned long aux_cls = getauxval(AT_DCACHEBSIZE);
  if (aux_cls > 0) cls = (size_t)aux_cls;
#endif
  if (cls == 0) {
    long conf = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
    cls = (conf > 0) ? (size_t)conf : RBIPC_CACHE_LINE;
  }
  s_profile.cache_line_size = cls;
  s_profile.destructive_size = (cls < RBIPC_CACHE_LINE) ? RBIPC_CACHE_LINE : cls;

  /* 3. System Default HugeTLB Page Size */
  size_t hps = 0;
  FILE *fp = fopen("/proc/meminfo", "r");
  if (fp) {
    char line[128];
    while (fgets(line, sizeof(line), fp)) {
      if (strncmp(line, "Hugepagesize:", 13) == 0) {
        unsigned long kb = 0;
        if (sscanf(line + 13, "%lu", &kb) == 1 && kb > 0) {
          hps = (size_t)kb * 1024ULL;
          break;
        }
      }
    }
    fclose(fp);
  }
  if (hps == 0) {
    if (ps >= 65536U) {
      hps = 512ULL * 1024ULL * 1024ULL; /* 512 MB for 64 KB page ARM64/ppc64le */
    } else if (ps >= 16384U) {
      hps = 32ULL * 1024ULL * 1024ULL;  /* 32 MB for 16 KB page Android/Apple */
    } else {
      hps = 2ULL * 1024ULL * 1024ULL;   /* 2 MB for 4 KB page x86/ARM */
    }
  }
  s_profile.huge_page_size = hps;

  /* 4. Cache Hierarchy (L2 and L3) */
  long l2 = sysconf(_SC_LEVEL2_CACHE_SIZE);
  long l3 = sysconf(_SC_LEVEL3_CACHE_SIZE);
  s_profile.l2_cache_size = (l2 > 0) ? (size_t)l2 : 0;
  s_profile.l3_cache_size = (l3 > 0) ? (size_t)l3 : 0;

  /* 5. Optimal Non-Temporal Streaming Threshold */
  size_t st = 0;
  if (s_profile.l3_cache_size > 0) {
    /* glibc policy: 3/4 of L3 cache size */
    st = (s_profile.l3_cache_size * 3) / 4;
  } else if (s_profile.l2_cache_size > 0) {
    st = s_profile.l2_cache_size;
  }
  if (st < (256ULL * 1024ULL)) {
    st = 256ULL * 1024ULL; /* Minimum 256 KB floor to avoid L1/L2 thrashing */
  }
  s_profile.stream_threshold = st;

  /* 6. Intel DSA DMA Offload Threshold */
  s_profile.dma_offload_threshold = 16384U; /* 16 KB break-even */

  /* 7. Online Processors */
  long cores = sysconf(_SC_NPROCESSORS_ONLN);
  s_profile.online_cores = (cores > 0) ? (uint32_t)cores : 1U;

  /* 8. Microarchitectural Spin Budget */
  s_profile.calibrated_spin_count = 48U;
#if defined(__x86_64__) || defined(_M_X64)
  /* On Skylake/Ice Lake where PAUSE takes 140 cycles, 16 pauses is ~2240 cycles (~700 ns) */
  unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
  unsigned int family = 0, model = 0;
  if (__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
    family = ((eax >> 8) & 0x0F) + ((eax >> 20) & 0xFF);
    model = ((eax >> 4) & 0x0F) | ((eax >> 12) & 0xF0);
    if (family == 6 && (model == 0x4E || model == 0x5E || model == 0x8E || model == 0x9E || model == 0x55)) {
      s_profile.calibrated_spin_count = 16U;
    }
  }
#endif

  /* 9. TSC / Timer Ticks Per Nanosecond */
  s_profile.tsc_ticks_per_ns = rbipc_detect_tsc_ticks_per_ns();
}

RBIPC_LEAF
const rbipc_hardware_profile_t *rbipc_get_hardware_profile(void) {
  int state = atomic_load_explicit(&s_profile_state, memory_order_acquire);
  if (RBIPC_LIKELY(state == 2)) {
    return &s_profile;
  }

  int expected = 0;
  if (atomic_compare_exchange_strong_explicit(&s_profile_state, &expected, 1,
                                              memory_order_acq_rel, memory_order_acquire)) {
    rbipc_hardware_profile_init();
    atomic_store_explicit(&s_profile_state, 2, memory_order_release);
  } else {
    while (atomic_load_explicit(&s_profile_state, memory_order_acquire) != 2) {
      rbipc_cpu_pause();
    }
  }

  return &s_profile;
}

RBIPC_LEAF size_t rbipc_hardware_cache_line_size(void) {
  return rbipc_get_hardware_profile()->cache_line_size;
}

RBIPC_LEAF size_t rbipc_hardware_page_size(void) {
  return rbipc_get_hardware_profile()->page_size;
}

RBIPC_LEAF size_t rbipc_hardware_huge_page_size(void) {
  return rbipc_get_hardware_profile()->huge_page_size;
}

RBIPC_LEAF size_t rbipc_hardware_streaming_threshold(void) {
  return rbipc_get_hardware_profile()->stream_threshold;
}

RBIPC_LEAF size_t rbipc_hardware_dma_threshold(void) {
  return rbipc_get_hardware_profile()->dma_offload_threshold;
}

RBIPC_LEAF uint32_t rbipc_hardware_online_cores(void) {
  return rbipc_get_hardware_profile()->online_cores;
}

RBIPC_LEAF uint64_t rbipc_hardware_tsc_ticks_per_ns(void) {
  return rbipc_get_hardware_profile()->tsc_ticks_per_ns;
}
