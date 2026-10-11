/**
 * @file rbipc_simd.c
 * @brief Implementation of SIMD-accelerated slot sequence scanning.
 *
 * @details Implements AVX2 and NEON vector comparison pipelines for scanning
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
#include "rbipc_predicate.h"

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#elif defined(__aarch64__)
#include <arm_neon.h>
#endif

/**
 * @brief Portable scalar fallback loop for scanning slot sequence progression.
 *
 * @param[in] slots  Slot descriptor array.
 * @param[in] mask   Ring capacity mask.
 * @param[in] t      Starting ticket.
 * @param[in] count  Maximum slots to inspect.
 * @param[in] offset Sequence offset (0 for vacant, 1 for ready).
 * @return Number of contiguous matching slots.
 */
static uint32_t rbipc_simd_scan_scalar(const rbipc_slot_t *slots, uint32_t mask,
                                       uint64_t t, uint32_t count,
                                       uint64_t offset) {
  uint32_t avail = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const rbipc_slot_t *slot = &slots[(t + i) & mask];
    uint64_t seq = atomic_load_explicit(&slot->sequence, memory_order_acquire);
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
__attribute__((target("avx2")))
#endif
/**
 * @brief 256-bit AVX2 accelerated slot sequence scanner.
 *
 * @param[in] slots  Slot descriptor array.
 * @param[in] mask   Ring capacity mask.
 * @param[in] t      Starting ticket.
 * @param[in] count  Maximum slots to inspect.
 * @param[in] offset Sequence offset (0 for vacant, 1 for ready).
 * @return Number of contiguous matching slots.
 */
static uint32_t rbipc_simd_scan_avx2(const rbipc_slot_t *slots, uint32_t mask,
                                     uint64_t t, uint32_t count,
                                     uint64_t offset) {
  uint32_t avail = 0;
  while (avail + 4 <= count) {
    uint64_t base_t = t + avail;
    uint32_t idx = (uint32_t)(base_t & mask);
    uint64_t s0, s1, s2, s3;
    if (idx + 4 <= mask + 1) {
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
    long long t0 = (long long)base_t + (long long)offset;
    __m256i vtgt = _mm256_set_epi64x(t0 + 3, t0 + 2, t0 + 1, t0);
    __m256i cmp = _mm256_cmpeq_epi64(vseq, vtgt);
    int match_mask = _mm256_movemask_pd((__m256d)cmp);

    if (match_mask == 0x0F) {
      avail += 4;
    } else {
      if ((match_mask & 0x01) == 0)
        return avail;
      if ((match_mask & 0x02) == 0)
        return avail + 1;
      if ((match_mask & 0x04) == 0)
        return avail + 2;
      return avail + 3;
    }
  }

  while (avail < count) {
    const rbipc_slot_t *slot = &slots[(t + avail) & mask];
    uint64_t seq = atomic_load_explicit(&slot->sequence, memory_order_acquire);
    if (rbipc_seq_diff(seq, t + avail + offset) == 0) {
      avail++;
    } else {
      break;
    }
  }
  return avail;
}
#elif defined(__aarch64__)
/**
 * @brief 128-bit ARM NEON accelerated slot sequence scanner.
 */
static uint32_t rbipc_simd_scan_neon(const rbipc_slot_t *slots, uint32_t mask,
                                     uint64_t t, uint32_t count,
                                     uint64_t offset) {
  uint32_t avail = 0;
  while (avail + 2 <= count) {
    uint64_t base_t = t + avail;
    uint32_t idx = (uint32_t)(base_t & mask);
    uint64_t s0, s1;
    if (idx + 2 <= mask + 1) {
      s0 = atomic_load_explicit(&slots[idx + 0].sequence, memory_order_acquire);
      s1 = atomic_load_explicit(&slots[idx + 1].sequence, memory_order_acquire);
    } else {
      s0 = atomic_load_explicit(&slots[(base_t + 0) & mask].sequence,
                                memory_order_acquire);
      s1 = atomic_load_explicit(&slots[(base_t + 1) & mask].sequence,
                                memory_order_acquire);
    }

    uint64x2_t vseq = {s0, s1};
    uint64x2_t vtgt = {base_t + 0 + offset, base_t + 1 + offset};
    uint64x2_t cmp = vceqq_u64(vseq, vtgt);
    if (vgetq_lane_u64(cmp, 0) == 0)
      return avail;
    if (vgetq_lane_u64(cmp, 1) == 0)
      return avail + 1;
    avail += 2;
  }

  if (avail < count) {
    const rbipc_slot_t *slot = &slots[(t + avail) & mask];
    uint64_t seq = atomic_load_explicit(&slot->sequence, memory_order_acquire);
    if (rbipc_seq_diff(seq, t + avail + offset) == 0) {
      avail++;
    }
  }
  return avail;
}
#endif

RBIPC_LEAF
uint32_t rbipc_simd_scan_vacant_slots(const rbipc_slot_t *slots, uint32_t mask,
                                      uint64_t t, uint32_t count) {
  if (RBIPC_UNLIKELY(!slots || count == 0))
    return 0;
#if defined(__x86_64__) || defined(_M_X64)
#if defined(__GNUC__) || defined(__clang__)
  if (__builtin_cpu_supports("avx2") && count >= 4) {
    return rbipc_simd_scan_avx2(slots, mask, t, count, 0);
  }
#endif
#elif defined(__aarch64__)
  if (count >= 2) {
    return rbipc_simd_scan_neon(slots, mask, t, count, 0);
  }
#endif
  return rbipc_simd_scan_scalar(slots, mask, t, count, 0);
}

RBIPC_LEAF
uint32_t rbipc_simd_scan_ready_slots(const rbipc_slot_t *slots, uint32_t mask,
                                     uint64_t t, uint32_t count) {
  if (RBIPC_UNLIKELY(!slots || count == 0))
    return 0;
#if defined(__x86_64__) || defined(_M_X64)
#if defined(__GNUC__) || defined(__clang__)
  if (__builtin_cpu_supports("avx2") && count >= 4) {
    return rbipc_simd_scan_avx2(slots, mask, t, count, 1);
  }
#endif
#elif defined(__aarch64__)
  if (count >= 2) {
    return rbipc_simd_scan_neon(slots, mask, t, count, 1);
  }
#endif
  return rbipc_simd_scan_scalar(slots, mask, t, count, 1);
}