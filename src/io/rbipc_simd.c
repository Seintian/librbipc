/**
 * @file rbipc_simd.c
 * @brief Implementation of SIMD-accelerated slot sequence scanning.
 *
 * @details Implements AVX-512, AVX2, and NEON vector comparison pipelines for scanning
 * contiguous sequences of slot descriptors to detect vacancy and read
 * readiness, with portable scalar fallback.
 *
 * @author Seintian
 * @date 2026
 * @copyright Apache License 2.0
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "rbipc_simd.h"
#include "rbipc_math.h"

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#elif defined(__aarch64__)
#include <arm_neon.h>
#endif

/**
 * @brief Portable scalar fallback loop for scanning slot sequence progression.
 *
 * @param[in] slots     Slot descriptor array.
 * @param[in] mask      Ring capacity mask.
 * @param[in] t         Starting ticket.
 * @param[in] count     Maximum slots to inspect.
 * @param[in] offset    Sequence offset (0 for vacant, 1 for ready).
 * @param[in] is_packed True if 64-bit packed sequence/len/state descriptors.
 * @return Number of contiguous matching slots.
 */
RBIPC_INTERNAL uint32_t rbipc_simd_scan_scalar(const rbipc_slot_t *slots, uint32_t mask,
                                               uint64_t t, uint32_t count,
                                               uint64_t offset, bool is_packed) {
  uint32_t avail = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const rbipc_slot_t *slot = &slots[(t + i) & mask];
    uint64_t raw = atomic_load_explicit(&slot->sequence, memory_order_acquire);
    uint64_t seq = is_packed ? (raw >> RBIPC_PACKED_SEQ_SHIFT) : raw;
    if (rbipc_seq_diff(seq, t + i + offset) == 0) {
      avail++;
    } else {
      break;
    }
  }
  return avail;
}

#if defined(__x86_64__) || defined(_M_X64)
#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("avx512f")))
#endif
RBIPC_INTERNAL uint32_t rbipc_simd_scan_avx512(const rbipc_slot_t *slots, uint32_t mask,
                                               uint64_t t, uint32_t count,
                                               uint64_t offset, bool is_packed) {
  uint32_t avail = 0;
  long long start_t = (long long)t + (long long)offset;
  __m512i vtgt = _mm512_set_epi64(start_t + 7, start_t + 6, start_t + 5, start_t + 4,
                                  start_t + 3, start_t + 2, start_t + 1, start_t);
  const __m512i vstep = _mm512_set1_epi64(8);

  while (avail + 8 <= count) {
    uint64_t base_t = t + avail;
    uint32_t idx = (uint32_t)(base_t & mask);
    RBIPC_PREFETCH(&slots[(base_t + 16) & mask].sequence, RBIPC_PREFETCH_READ,
                   RBIPC_PREFETCH_LOCALITY_HIGH);
    uint64_t s[8];
    if (RBIPC_LIKELY(idx + 8 <= mask + 1)) {
      for (int k = 0; k < 8; ++k) {
        s[k] = atomic_load_explicit(&slots[idx + k].sequence, memory_order_acquire);
      }
    } else {
      for (int k = 0; k < 8; ++k) {
        s[k] = atomic_load_explicit(&slots[(base_t + (uint64_t)k) & mask].sequence,
                                    memory_order_acquire);
      }
    }

    __m512i vseq = _mm512_set_epi64((long long)s[7], (long long)s[6], (long long)s[5],
                                    (long long)s[4], (long long)s[3], (long long)s[2],
                                    (long long)s[1], (long long)s[0]);
    if (is_packed) {
      vseq = _mm512_srli_epi64(vseq, RBIPC_PACKED_SEQ_SHIFT);
    }
    __mmask8 match_mask = _mm512_cmpeq_epi64_mask(vseq, vtgt);

    if (match_mask == 0xFFU) {
      avail += 8;
      vtgt = _mm512_add_epi64(vtgt, vstep);
    } else {
      uint32_t mismatch_bits = ((uint32_t)(~match_mask)) & 0xFFU;
      return avail + (uint32_t)__builtin_ctz(mismatch_bits);
    }
  }

  if (avail < count) {
    avail += rbipc_simd_scan_scalar(slots, mask, t + avail, count - avail, offset, is_packed);
  }
  return avail;
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("avx2")))
#endif
/**
 * @brief 256-bit AVX2 accelerated slot sequence scanner.
 *
 * @param[in] slots     Slot descriptor array.
 * @param[in] mask      Ring capacity mask.
 * @param[in] t         Starting ticket.
 * @param[in] count     Maximum slots to inspect.
 * @param[in] offset    Sequence offset (0 for vacant, 1 for ready).
 * @param[in] is_packed True if 64-bit packed sequence/len/state descriptors.
 * @return Number of contiguous matching slots.
 */
RBIPC_INTERNAL uint32_t rbipc_simd_scan_avx2(const rbipc_slot_t *slots, uint32_t mask,
                                             uint64_t t, uint32_t count,
                                             uint64_t offset, bool is_packed) {
  uint32_t avail = 0;
  long long start_t = (long long)t + (long long)offset;
  __m256i vtgt = _mm256_set_epi64x(start_t + 3, start_t + 2, start_t + 1, start_t);
  const __m256i vstep = _mm256_set1_epi64x(4);

  while (avail + 4 <= count) {
    uint64_t base_t = t + avail;
    uint32_t idx = (uint32_t)(base_t & mask);
    RBIPC_PREFETCH(&slots[(base_t + 8) & mask].sequence, RBIPC_PREFETCH_READ,
                   RBIPC_PREFETCH_LOCALITY_HIGH);
    uint64_t s0, s1, s2, s3;
    if (RBIPC_LIKELY(idx + 4 <= mask + 1)) {
      s0 = atomic_load_explicit(&slots[idx + 0].sequence, memory_order_acquire);
      s1 = atomic_load_explicit(&slots[idx + 1].sequence, memory_order_acquire);
      s2 = atomic_load_explicit(&slots[idx + 2].sequence, memory_order_acquire);
      s3 = atomic_load_explicit(&slots[idx + 3].sequence, memory_order_acquire);
    } else {
      s0 = atomic_load_explicit(&slots[(base_t + 0) & mask].sequence,
                                memory_order_acquire);
      s1 = atomic_load_explicit(&slots[(base_t + 1) & mask].sequence,
                                memory_order_acquire);
      s2 = atomic_load_explicit(&slots[(base_t + 2) & mask].sequence,
                                memory_order_acquire);
      s3 = atomic_load_explicit(&slots[(base_t + 3) & mask].sequence,
                                memory_order_acquire);
    }

    __m256i vseq = _mm256_set_epi64x((long long)s3, (long long)s2,
                                     (long long)s1, (long long)s0);
    if (is_packed) {
      vseq = _mm256_srli_epi64(vseq, RBIPC_PACKED_SEQ_SHIFT);
    }
    __m256i cmp = _mm256_cmpeq_epi64(vseq, vtgt);
    uint32_t match_mask = (uint32_t)_mm256_movemask_pd((__m256d)cmp);

    if (match_mask == 0x0FU) {
      avail += 4;
      vtgt = _mm256_add_epi64(vtgt, vstep);
    } else {
      uint32_t mismatch_bits = (~match_mask) & 0x0FU;
      return avail + (uint32_t)__builtin_ctz(mismatch_bits);
    }
  }

  if (avail < count) {
    avail += rbipc_simd_scan_scalar(slots, mask, t + avail, count - avail, offset, is_packed);
  }
  return avail;
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("avx")))
#endif
/**
 * @brief 256-bit AVX1 accelerated slot sequence scanner (dual 128-bit VEX-encoded lanes).
 *
 * @details Utilizes 128-bit VEX-encoded vector integer comparisons and a 256-bit
 * combined mask via _mm256_set_m128d and _mm256_movemask_pd, scanning 4 slots per cycle
 * on first-generation AVX architectures (Sandy Bridge / Ivy Bridge) where AVX2 is absent.
 *
 * @param[in] slots     Slot descriptor array.
 * @param[in] mask      Ring capacity mask.
 * @param[in] t         Starting ticket.
 * @param[in] count     Maximum slots to inspect.
 * @param[in] offset    Sequence offset (0 for vacant, 1 for ready).
 * @param[in] is_packed True if 64-bit packed sequence/len/state descriptors.
 * @return Number of contiguous matching slots.
 */
RBIPC_INTERNAL uint32_t rbipc_simd_scan_avx(const rbipc_slot_t *slots, uint32_t mask,
                                            uint64_t t, uint32_t count,
                                            uint64_t offset, bool is_packed) {
  uint32_t avail = 0;
  long long start_t = (long long)t + (long long)offset;
  __m128i vtgt0 = _mm_set_epi64x(start_t + 1, start_t + 0);
  __m128i vtgt1 = _mm_set_epi64x(start_t + 3, start_t + 2);
  const __m128i vstep = _mm_set1_epi64x(4);

  while (avail + 4 <= count) {
    uint64_t base_t = t + avail;
    uint32_t idx = (uint32_t)(base_t & mask);
    RBIPC_PREFETCH(&slots[(base_t + 8) & mask].sequence, RBIPC_PREFETCH_READ,
                   RBIPC_PREFETCH_LOCALITY_HIGH);
    uint64_t s0, s1, s2, s3;
    if (RBIPC_LIKELY(idx + 4 <= mask + 1)) {
      s0 = atomic_load_explicit(&slots[idx + 0].sequence, memory_order_acquire);
      s1 = atomic_load_explicit(&slots[idx + 1].sequence, memory_order_acquire);
      s2 = atomic_load_explicit(&slots[idx + 2].sequence, memory_order_acquire);
      s3 = atomic_load_explicit(&slots[idx + 3].sequence, memory_order_acquire);
    } else {
      s0 = atomic_load_explicit(&slots[(base_t + 0) & mask].sequence,
                                memory_order_acquire);
      s1 = atomic_load_explicit(&slots[(base_t + 1) & mask].sequence,
                                memory_order_acquire);
      s2 = atomic_load_explicit(&slots[(base_t + 2) & mask].sequence,
                                memory_order_acquire);
      s3 = atomic_load_explicit(&slots[(base_t + 3) & mask].sequence,
                                memory_order_acquire);
    }

    __m128i vseq0 = _mm_set_epi64x((long long)s1, (long long)s0);
    __m128i vseq1 = _mm_set_epi64x((long long)s3, (long long)s2);
    if (is_packed) {
      vseq0 = _mm_srli_epi64(vseq0, RBIPC_PACKED_SEQ_SHIFT);
      vseq1 = _mm_srli_epi64(vseq1, RBIPC_PACKED_SEQ_SHIFT);
    }
    __m128i cmp0 = _mm_cmpeq_epi64(vseq0, vtgt0);
    __m128i cmp1 = _mm_cmpeq_epi64(vseq1, vtgt1);
    __m256d combined = _mm256_set_m128d((__m128d)cmp1, (__m128d)cmp0);
    uint32_t match_mask = (uint32_t)_mm256_movemask_pd(combined);

    if (match_mask == 0x0FU) {
      avail += 4;
      vtgt0 = _mm_add_epi64(vtgt0, vstep);
      vtgt1 = _mm_add_epi64(vtgt1, vstep);
    } else {
      uint32_t mismatch_bits = (~match_mask) & 0x0FU;
      return avail + (uint32_t)__builtin_ctz(mismatch_bits);
    }
  }

  if (avail < count) {
    avail += rbipc_simd_scan_scalar(slots, mask, t + avail, count - avail, offset, is_packed);
  }
  return avail;
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("sse4.1")))
#endif
/**
 * @brief 128-bit SSE4.1 accelerated slot sequence scanner (2 slots per vector).
 */
RBIPC_INTERNAL uint32_t rbipc_simd_scan_sse41(const rbipc_slot_t *slots, uint32_t mask,
                                              uint64_t t, uint32_t count,
                                              uint64_t offset, bool is_packed) {
  uint32_t avail = 0;
  long long start_t = (long long)t + (long long)offset;
  __m128i vtgt = _mm_set_epi64x(start_t + 1, start_t);
  const __m128i vstep = _mm_set1_epi64x(2);

  while (avail + 2 <= count) {
    uint64_t base_t = t + avail;
    uint32_t idx = (uint32_t)(base_t & mask);
    
    /* Software prefetch lookahead for subsequent 2 slot cache lines */
    RBIPC_PREFETCH(&slots[(base_t + 4) & mask].sequence, RBIPC_PREFETCH_READ,
                   RBIPC_PREFETCH_LOCALITY_HIGH);

    uint64_t s0, s1;
    if (RBIPC_LIKELY(idx + 2 <= mask + 1)) {
      s0 = atomic_load_explicit(&slots[idx + 0].sequence, memory_order_acquire);
      s1 = atomic_load_explicit(&slots[idx + 1].sequence, memory_order_acquire);
    } else {
      s0 = atomic_load_explicit(&slots[(base_t + 0) & mask].sequence,
                                memory_order_acquire);
      s1 = atomic_load_explicit(&slots[(base_t + 1) & mask].sequence,
                                memory_order_acquire);
    }

    __m128i vseq = _mm_set_epi64x((long long)s1, (long long)s0);
    if (is_packed) {
      vseq = _mm_srli_epi64(vseq, RBIPC_PACKED_SEQ_SHIFT);
    }
    __m128i cmp = _mm_cmpeq_epi64(vseq, vtgt);
    uint32_t match_mask = (uint32_t)_mm_movemask_pd((__m128d)cmp);

    if (match_mask == 0x03U) {
      avail += 2;
      vtgt = _mm_add_epi64(vtgt, vstep);
    } else {
      uint32_t mismatch_bits = (~match_mask) & 0x03U;
      return avail + (uint32_t)__builtin_ctz(mismatch_bits);
    }
  }

  if (avail < count) {
    avail += rbipc_simd_scan_scalar(slots, mask, t + avail, count - avail, offset, is_packed);
  }
  return avail;
}

#elif defined(__aarch64__)
/**
 * @brief 128-bit ARM NEON accelerated slot sequence scanner.
 */
static uint32_t rbipc_simd_scan_neon(const rbipc_slot_t *slots, uint32_t mask,
                                     uint64_t t, uint32_t count,
                                     uint64_t offset, bool is_packed) {
  uint32_t avail = 0;
  while (avail + 2 <= count) {
    uint64_t base_t = t + avail;
    uint32_t idx = (uint32_t)(base_t & mask);
    uint64_t s0, s1;
    if (RBIPC_LIKELY(idx + 2 <= mask + 1)) {
      s0 = atomic_load_explicit(&slots[idx + 0].sequence, memory_order_acquire);
      s1 = atomic_load_explicit(&slots[idx + 1].sequence, memory_order_acquire);
    } else {
      s0 = atomic_load_explicit(&slots[(base_t + 0) & mask].sequence,
                                memory_order_acquire);
      s1 = atomic_load_explicit(&slots[(base_t + 1) & mask].sequence,
                                memory_order_acquire);
    }

    uint64x2_t vseq = {s0, s1};
    if (is_packed) {
      vseq = vshrq_n_u64(vseq, RBIPC_PACKED_SEQ_SHIFT);
    }
    uint64x2_t vtgt = {base_t + 0 + offset, base_t + 1 + offset};
    uint64x2_t cmp = vceqq_u64(vseq, vtgt);
    if (vgetq_lane_u64(cmp, 0) == 0)
      return avail;
    if (vgetq_lane_u64(cmp, 1) == 0)
      return avail + 1;
    avail += 2;
  }

  if (avail < count) {
    avail += rbipc_simd_scan_scalar(slots, mask, t + avail, count - avail, offset, is_packed);
  }
  return avail;
}
#endif

/**
 * @brief Unified slot scanner with hardware acceleration and scalar fallback.
 */
RBIPC_INLINE uint32_t rbipc_simd_scan_slots(const rbipc_slot_t *slots,
                                            uint32_t mask, uint64_t t,
                                            uint32_t count, uint64_t offset,
                                            bool is_packed) {
  if (RBIPC_UNLIKELY(!slots || count == 0))
    return 0;
#if defined(__x86_64__) || defined(_M_X64)
#if defined(__GNUC__) || defined(__clang__)
  if (__builtin_cpu_supports("avx512f") && count >= 8) {
    return rbipc_simd_scan_avx512(slots, mask, t, count, offset, is_packed);
  }
  if (__builtin_cpu_supports("avx2") && count >= 4) {
    return rbipc_simd_scan_avx2(slots, mask, t, count, offset, is_packed);
  }
  if (__builtin_cpu_supports("avx") && count >= 4) {
    return rbipc_simd_scan_avx(slots, mask, t, count, offset, is_packed);
  }
  if (__builtin_cpu_supports("sse4.1") && count >= 2) {
    return rbipc_simd_scan_sse41(slots, mask, t, count, offset, is_packed);
  }
#endif
#elif defined(__aarch64__)
  if (count >= 2) {
    return rbipc_simd_scan_neon(slots, mask, t, count, offset, is_packed);
  }
#endif
  return rbipc_simd_scan_scalar(slots, mask, t, count, offset, is_packed);
}

RBIPC_INTERNAL RBIPC_LEAF
uint32_t rbipc_simd_scan_vacant_slots(const rbipc_slot_t *slots, uint32_t mask,
                                      uint64_t t, uint32_t count) {
  return rbipc_simd_scan_slots(slots, mask, t, count, 0, false);
}

RBIPC_INTERNAL RBIPC_LEAF
uint32_t rbipc_simd_scan_vacant_slots_ex(const rbipc_slot_t *slots, uint32_t mask,
                                         uint64_t t, uint32_t count, bool is_packed) {
  return rbipc_simd_scan_slots(slots, mask, t, count, 0, is_packed);
}

RBIPC_INTERNAL RBIPC_LEAF
uint32_t rbipc_simd_scan_ready_slots(const rbipc_slot_t *slots, uint32_t mask,
                                     uint64_t t, uint32_t count) {
  return rbipc_simd_scan_slots(slots, mask, t, count, 1, false);
}

RBIPC_INTERNAL RBIPC_LEAF
uint32_t rbipc_simd_scan_ready_slots_ex(const rbipc_slot_t *slots, uint32_t mask,
                                        uint64_t t, uint32_t count, bool is_packed) {
  return rbipc_simd_scan_slots(slots, mask, t, count, 1, is_packed);
}
