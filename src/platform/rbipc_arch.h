/**
 * @file rbipc_arch.h
 * @brief Architecture-specific hardware intrinsics and high-resolution timing
 * primitives.
 *
 * @details Encapsulates CPU pipeline pause instructions across x86_64, AArch64,
 * ARM, and RISC-V architectures to optimize memory ordering pipeline stalls
 * during spin-wait phases and prevent speculative pipeline thrashing, alongside
 * POSIX CLOCK_MONOTONIC timing wrappers.
 *
 * @author Seintian
 * @date 2026
 * @copyright Apache License 2.0
 */

#ifndef RBIPC_ARCH_H
#define RBIPC_ARCH_H

#include "rbipc.h"
#include "rbipc_attr.h"

#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#if defined(__linux__)
#include <sys/auxv.h>
#endif

/**
 * @brief Query current high-resolution monotonic clock timestamp in
 * nanoseconds.
 *
 * @details Reads Linux POSIX CLOCK_MONOTONIC clock source. Monotonic timestamps
 * are strictly immune to wall-clock time adjustments, NTP skews, and
 * leap-second discontinuities, making them ideal for deadline calculations and
 * performance latency profiling.
 *
 * @return 64-bit integer timestamp in nanoseconds.
 */
RBIPC_INLINE RBIPC_NODISCARD uint64_t rbipc_clock_monotonic_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ((uint64_t)ts.tv_sec * RBIPC_NSEC_PER_SEC) + (uint64_t)ts.tv_nsec;
}

/**
 * @def RBIPC_PAGE_SIZE_DEFAULT
 * @brief Default virtual memory page size fallback in bytes (4096).
 */
#define RBIPC_PAGE_SIZE_DEFAULT 4096U

/**
 * @brief Query system virtual memory page size with atomic process-local caching.
 *
 * @details Queries sysconf(_SC_PAGESIZE) on first invocation and caches the result
 * atomically to eliminate subsequent system calls. Falls back to RBIPC_PAGE_SIZE_DEFAULT bytes if sysconf fails.
 *
 * @return System virtual memory page size in bytes.
 */
RBIPC_INLINE RBIPC_NODISCARD size_t rbipc_page_size(void) {
  static _Atomic size_t s_page_size = 0;
  size_t ps = atomic_load_explicit(&s_page_size, memory_order_acquire);
  if (RBIPC_LIKELY(ps != 0)) {
    return ps;
  }
#if defined(__linux__)
  unsigned long aux_ps = getauxval(AT_PAGESZ);
  if (aux_ps > 0) {
    ps = (size_t)aux_ps;
  }
#endif
  if (ps == 0) {
    long raw = sysconf(_SC_PAGESIZE);
    ps = (raw > 0) ? (size_t)raw : RBIPC_PAGE_SIZE_DEFAULT;
  }
  atomic_store_explicit(&s_page_size, ps, memory_order_release);
  return ps;
}

/**
 * @brief Query system default HugeTLB page size in bytes.
 * @details Probes /proc/meminfo or /sys/kernel/mm/hugepages, dynamically resolving
 * 2 MB on x86, 32 MB on 16 KB ARM64, 512 MB on 64 KB ARM64, or 16 MB on ppc64le.
 */
RBIPC_INLINE RBIPC_NODISCARD size_t rbipc_huge_page_size(void) {
  static _Atomic size_t s_huge_page_size = 0;
  size_t hps = atomic_load_explicit(&s_huge_page_size, memory_order_acquire);
  if (RBIPC_LIKELY(hps != 0)) {
    return hps;
  }
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
    size_t base_ps = rbipc_page_size();
    if (base_ps >= 65536U) {
      hps = 512ULL * 1024ULL * 1024ULL; /* 512 MB for 64 KB ARM/PowerPC */
    } else if (base_ps >= 16384U) {
      hps = 32ULL * 1024ULL * 1024ULL;  /* 32 MB for 16 KB Android/Apple */
    } else {
      hps = 2ULL * 1024ULL * 1024ULL;   /* 2 MB standard x86/ARM */
    }
  }
  atomic_store_explicit(&s_huge_page_size, hps, memory_order_release);
  return hps;
}

/**
 * @brief Query physical L1 data cache line size in bytes with runtime hardware validation.
 */
RBIPC_INLINE RBIPC_NODISCARD size_t rbipc_cache_line_size(void) {
  static _Atomic size_t s_cache_line = 0;
  size_t cls = atomic_load_explicit(&s_cache_line, memory_order_acquire);
  if (RBIPC_LIKELY(cls != 0)) {
    return cls;
  }
#if defined(__linux__) && defined(AT_DCACHEBSIZE)
  unsigned long aux_cls = getauxval(AT_DCACHEBSIZE);
  if (aux_cls > 0) {
    cls = (size_t)aux_cls;
  }
#endif
  if (cls == 0) {
    long conf = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
    cls = (conf > 0) ? (size_t)conf : RBIPC_CACHE_LINE;
  }
  atomic_store_explicit(&s_cache_line, cls, memory_order_release);
  return cls;
}

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) ||             \
    defined(_M_IX86)
#include <cpuid.h>
#include <immintrin.h>
#include <x86intrin.h>

/**
 * @brief Query Time-Stamp Counter (TSC) tick value.
 * @return 64-bit current TSC cycle counter.
 */
RBIPC_INLINE RBIPC_NODISCARD uint64_t rbipc_rdtsc(void) {
  return __rdtsc();
}

/**
 * @brief Dynamic CPUID check for Intel WAITPKG instructions (UMONITOR, UMWAIT, TPAUSE).
 * @return True if host CPU supports WAITPKG (Intel Tremont, Alder Lake+, AMD Zen 5+).
 */
RBIPC_INLINE RBIPC_NODISCARD bool rbipc_cpu_has_waitpkg(void) {
  static _Atomic int cached = -1;
  int c = atomic_load_explicit(&cached, memory_order_acquire);
  if (RBIPC_LIKELY(c != -1)) {
    return c == 1;
  }
  unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
  int res = 0;
  if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) {
    res = ((ecx & (1U << 5U)) != 0U) ? 1 : 0;
  }
  atomic_store_explicit(&cached, res, memory_order_release);
  return res == 1;
}

/**
 * @brief Arm processor hardware address monitor on a memory address.
 * @param[in] addr Target memory address / cache line to monitor.
 */
RBIPC_INLINE void rbipc_cpu_umonitor(const void *addr) {
  __asm__ __volatile__("umonitor %0" :: "r"(addr) : "memory");
}

/**
 * @brief Enter low-power user-mode substate C0.1 or C0.2 until address invalidation.
 * @param[in] state        Substate target (1 for C0.1 fast wake, 0 for C0.2 deep power save).
 * @param[in] tsc_deadline Monotonic TSC cycle count expiration deadline.
 * @return Carry flag from instruction (0 on address invalidate/event, 1 on timeout expiration).
 */
RBIPC_INLINE uint8_t rbipc_cpu_umwait(uint32_t state, uint64_t tsc_deadline) {
  uint8_t cf = 0;
  __asm__ __volatile__("umwait %1"
                       : "=@ccc"(cf)
                       : "r"(state), "a"((uint32_t)tsc_deadline),
                         "d"((uint32_t)(tsc_deadline >> 32U))
                       : "memory");
  return cf;
}

/**
 * @brief Timed low-power pause without address monitoring.
 * @param[in] state        Substate target (1 for C0.1, 0 for C0.2).
 * @param[in] tsc_deadline Monotonic TSC cycle count expiration deadline.
 * @return Carry flag from instruction (0 on event, 1 on timeout expiration).
 */
RBIPC_INLINE uint8_t rbipc_cpu_tpause(uint32_t state, uint64_t tsc_deadline) {
  uint8_t cf = 0;
  __asm__ __volatile__("tpause %1"
                       : "=@ccc"(cf)
                       : "r"(state), "a"((uint32_t)tsc_deadline),
                         "d"((uint32_t)(tsc_deadline >> 32U))
                       : "memory");
  return cf;
}

#else

RBIPC_INLINE RBIPC_NODISCARD uint64_t rbipc_rdtsc(void) {
  return rbipc_clock_monotonic_ns();
}

RBIPC_INLINE RBIPC_NODISCARD bool rbipc_cpu_has_waitpkg(void) {
  return false;
}

#endif

#if defined(__aarch64__)
#include <sys/auxv.h>
#ifndef HWCAP2_WFXT
#define HWCAP2_WFXT (1UL << 31)
#endif

/**
 * @brief Dynamic runtime check for ARM64 FEAT_WFxT (WFET instruction).
 * @return True if host CPU supports WFET (ARMv8.7-A / ARMv9.2-A+).
 */
RBIPC_INLINE RBIPC_NODISCARD bool rbipc_cpu_has_wfxt(void) {
  static _Atomic int cached = -1;
  int c = atomic_load_explicit(&cached, memory_order_acquire);
  if (RBIPC_LIKELY(c != -1)) {
    return c == 1;
  }
  unsigned long hwcap2 = getauxval(AT_HWCAP2);
  int res = ((hwcap2 & HWCAP2_WFXT) != 0) ? 1 : 0;
  atomic_store_explicit(&cached, res, memory_order_release);
  return res == 1;
}

/**
 * @brief Read ARM64 virtual system counter frequency (CNTFRQ_EL0).
 * @return Virtual counter frequency in Hz.
 */
RBIPC_INLINE RBIPC_NODISCARD uint64_t rbipc_arm_cntfrq(void) {
  uint64_t freq = 0;
  __asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(freq));
  return freq;
}

/**
 * @brief Read ARM64 virtual system counter (CNTVCT_EL0).
 * @return Current virtual counter value.
 */
RBIPC_INLINE RBIPC_NODISCARD uint64_t rbipc_arm_cntvct(void) {
  uint64_t cnt = 0;
  __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(cnt));
  return cnt;
}

#else

RBIPC_INLINE RBIPC_NODISCARD bool rbipc_cpu_has_wfxt(void) {
  return false;
}

#endif

/**
 * @brief Emit a hardware processor hint indicating an execution pause or
 * spin-wait loop.
 *
 * @details
 * - On x86/x86_64: Emits `_mm_pause()` (PAUSE instruction), de-pipelining
 * memory order violations.
 * - On AArch64/ARM: Emits `yield` (HINT #1) to signal a spin-wait state without
 * pipeline-flushing overhead (unlike ISB).
 * - On RISC-V: Emits `pause` instruction.
 * - Fallback: Emits a compiler memory clobber barrier without hardware
 * instruction.
 *
 * @note In librbipc passive zero-spin mode, execution suspends via sys_futex
 * rather than spinning.
 */
RBIPC_INLINE void rbipc_cpu_pause(void) {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) ||             \
    defined(_M_IX86)
  _mm_pause();
#elif defined(__aarch64__) || defined(__arm__)
  __asm__ __volatile__("yield\n" ::: "memory");
#elif defined(__riscv)
  __asm__ __volatile__("pause\n" ::: "memory");
#else
  __asm__ __volatile__("" ::: "memory");
#endif
}

/* ============================================================================
 * Checksum & Hardware-Accelerated CRC32C (Castagnoli 0x82F63B78)
 * ============================================================================
 */

#include <stddef.h>

#include "rbipc_crc32c_tables.h"

/**
 * @brief Unaligned integer types for safe byte-aligned memory dereferencing.
 */
typedef uint64_t __attribute__((aligned(1))) rbipc_unaligned_u64;
typedef uint32_t __attribute__((aligned(1))) rbipc_unaligned_u32;
typedef uint16_t __attribute__((aligned(1))) rbipc_unaligned_u16;

#if defined(__x86_64__) || defined(__i386__)
/**
 * @brief Hardware-accelerated CRC32C using x86 SSE4.2 CRC32 instructions.
 *
 * @param[in] crc  Initial or running CRC32C accumulator value.
 * @param[in] data Buffer pointer.
 * @param[in] len  Buffer size in bytes.
 * @return Updated 32-bit CRC32C accumulator.
 */
__attribute__((target("sse4.2"))) static inline uint32_t
rbipc_crc32c_sse42(uint32_t crc, const void *data, size_t len) {
  const uint8_t *p = (const uint8_t *)data;
  while (len >= 8) {
    uint64_t val = *(const rbipc_unaligned_u64 *)p;
    crc = (uint32_t)__builtin_ia32_crc32di(crc, val);
    p += 8;
    len -= 8;
  }
  if (len >= 4) {
    uint32_t val = *(const rbipc_unaligned_u32 *)p;
    crc = __builtin_ia32_crc32si(crc, val);
    p += 4;
    len -= 4;
  }
  if (len >= 2) {
    uint16_t val = *(const rbipc_unaligned_u16 *)p;
    crc = __builtin_ia32_crc32hi(crc, val);
    p += 2;
    len -= 2;
  }
  if (len > 0) {
    crc = __builtin_ia32_crc32qi(crc, *p);
  }
  return crc;
}
#endif

#if defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
/**
 * @brief Hardware-accelerated CRC32C using ARMv8-A CRC32 instructions.
 *
 * @param[in] crc  Initial or running CRC32C accumulator value.
 * @param[in] data Buffer pointer.
 * @param[in] len  Buffer size in bytes.
 * @return Updated 32-bit CRC32C accumulator.
 */
static inline uint32_t rbipc_crc32c_arm(uint32_t crc, const void *data,
                                        size_t len) {
  const uint8_t *p = (const uint8_t *)data;
  while (len >= 8) {
    uint64_t val = *(const rbipc_unaligned_u64 *)p;
    crc = __builtin_arm_crc32cd(crc, val);
    p += 8;
    len -= 8;
  }
  if (len >= 4) {
    uint32_t val = *(const rbipc_unaligned_u32 *)p;
    crc = __builtin_arm_crc32cw(crc, val);
    p += 4;
    len -= 4;
  }
  if (len >= 2) {
    uint16_t val = *(const rbipc_unaligned_u16 *)p;
    crc = __builtin_arm_crc32ch(crc, val);
    p += 2;
    len -= 2;
  }
  if (len > 0) {
    crc = __builtin_arm_crc32cb(crc, *p);
  }
  return crc;
}
#endif

/**
 * @brief High-throughput software CRC32C fallback via Kounavis & Berry Slicing-by-8 algorithm.
 *
 * @details Traditional table-driven CRC algorithms process data byte-by-byte, introducing a rigid
 * data dependency chain where each lookup requires the completed result of the preceding byte.
 * On modern pipelined superscalar architectures, this serial execution stalls execution ports
 * and limits processing throughput to ~300-450 MB/s.
 *
 * The Slicing-by-8 paradigm eliminates this intra-iteration dependency by decomposing 8 consecutive
 * input bytes across 8 precomputed 256-entry lookup tables (8 KiB total). Within each 8-byte step,
 * all 8 lookups are mutually independent and can be issued concurrently by out-of-order execution
 * units:
 * \f[
 *   \begin{aligned}
 *     w_1 &= \text{crc} \oplus \text{read\_u32}(p) \\
 *     w_2 &= \text{read\_u32}(p + 4) \\
 *     \text{crc} &= T_7[w_1 \ \& \ \text{0xFF}] \oplus T_6[(w_1 \gg 8) \ \& \ \text{0xFF}]
 *                 \oplus T_5[(w_1 \gg 16) \ \& \ \text{0xFF}] \oplus T_4[w_1 \gg 24] \\
 *                &\oplus T_3[w_2 \ \& \ \text{0xFF}] \oplus T_2[(w_2 \gg 8) \ \& \ \text{0xFF}]
 *                 \oplus T_1[(w_2 \gg 16) \ \& \ \text{0xFF}] \oplus T_0[w_2 \gg 24]
 *   \end{aligned}
 * \f]
 * The entire 8 KiB table is retained in L1 data cache across calls, guaranteeing 0 cache misses.
 * Trailing bytes (< 8) are resolved via single-byte table lookups using Table 0.
 *
 * Reference:
 * - Michael E. Kounavis and Frank L. Berry (Intel Research, 2005),
 *   "Novel Table Lookup-Based Techniques for High-Performance CRC Generation",
 *   IEEE Symposium on Computers and Communications (ISCC 2005).
 *
 * @param[in] crc  Initial or running CRC32C accumulator value.
 * @param[in] data Buffer pointer.
 * @param[in] len  Buffer size in bytes.
 * @return Updated 32-bit CRC32C accumulator.
 */
static inline uint32_t rbipc_crc32c_sw(uint32_t crc, const void *data,
                                       size_t len) {
  const uint8_t *p = (const uint8_t *)data;

  while (len >= 8) {
    uint32_t one = crc ^ *(const rbipc_unaligned_u32 *)p;
    uint32_t two = *(const rbipc_unaligned_u32 *)(p + 4);
    crc = rbipc_crc32c_table[7][one & 0xFFU] ^
          rbipc_crc32c_table[6][(one >> 8U) & 0xFFU] ^
          rbipc_crc32c_table[5][(one >> 16U) & 0xFFU] ^
          rbipc_crc32c_table[4][one >> 24U] ^
          rbipc_crc32c_table[3][two & 0xFFU] ^
          rbipc_crc32c_table[2][(two >> 8U) & 0xFFU] ^
          rbipc_crc32c_table[1][(two >> 16U) & 0xFFU] ^
          rbipc_crc32c_table[0][two >> 24U];
    p += 8;
    len -= 8;
  }

  while (len > 0) {
    crc = rbipc_crc32c_table[0][(crc ^ *p++) & 0xFFU] ^ (crc >> 8U);
    len--;
  }

  return crc;
}

/**
 * @brief Query whether hardware-accelerated CRC32C is supported on the host CPU.
 *
 * @return True if hardware CRC32C instructions are available; false otherwise.
 */
RBIPC_INLINE RBIPC_NODISCARD bool rbipc_cpu_has_crc32c(void) {
#if defined(__x86_64__) || defined(__i386__)
  return __builtin_cpu_supports("sse4.2") != 0;
#elif defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
  return true;
#else
  return false;
#endif
}

/**
 * @brief Incremental/streaming Castagnoli CRC32C update over a memory buffer.
 *
 * @details Computes or updates the CRC32C accumulator using the fastest available
 * hardware instruction set on the host platform, falling back to the precomputed
 * 256-entry table implementation if hardware acceleration is unavailable.
 *
 * @note This function operates directly on the uncomplemented CRC accumulator.
 * To compute standard RFC 3720 CRC32C, initialize @p crc to 0xFFFFFFFFU and
 * invert the final result with `~`, or call @ref rbipc_crc32c directly.
 *
 * @param[in] crc  Running CRC32C accumulator value.
 * @param[in] data Buffer pointer.
 * @param[in] len  Buffer size in bytes.
 * @return Updated 32-bit CRC32C accumulator.
 */
RBIPC_INLINE RBIPC_PURE uint32_t rbipc_crc32c_update(uint32_t crc,
                                                     const void *data,
                                                     size_t len) {
#if defined(__x86_64__) || defined(__i386__)
  if (RBIPC_LIKELY(rbipc_cpu_has_crc32c())) {
    return rbipc_crc32c_sse42(crc, data, len);
  }
#elif defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
  return rbipc_crc32c_arm(crc, data, len);
#endif
  return rbipc_crc32c_sw(crc, data, len);
}

/**
 * @brief Compute standard Castagnoli CRC32C over a memory region with hardware
 * acceleration.
 *
 * @details Conforms to RFC 3720 (iSCSI) standard Castagnoli CRC32C checksum
 * specifications (polynomial 0x1EDC6F41, reflected 0x82F63B78, initial value
 * 0xFFFFFFFF, final XOR 0xFFFFFFFF).
 *
 * @param[in] data Buffer pointer.
 * @param[in] len  Buffer size in bytes.
 * @return 32-bit CRC32C checksum.
 */
RBIPC_INLINE RBIPC_PURE uint32_t rbipc_crc32c(const void *data, size_t len) {
  return ~rbipc_crc32c_update(0xFFFFFFFFU, data, len);
}

#endif /* RBIPC_ARCH_H */
