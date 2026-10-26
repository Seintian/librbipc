/**
 * @file rbipc_stream.c
 * @brief Implementation of non-temporal cache-bypassing memory streaming.
 *
 * @details Implements streaming memory copy routines using AVX2 and SSE2
 * streaming stores, along with high-level streaming transmission wrappers.
 *
 * @author Seintian
 * @date 2026
 * @copyright Apache License 2.0
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "rbipc_stream.h"

#include <string.h>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>

#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("avx512f")))
static void rbipc_stream_copy_avx512_blocks(uint8_t *RBIPC_RESTRICT d,
                                            const uint8_t *RBIPC_RESTRICT s,
                                            size_t blocks) {
  size_t i = 0;
  /* Unroll by 2 (128 bytes = 2 full 64-byte write-combining cache lines) */
  for (; i + 2 <= blocks; i += 2) {
    __m512i v0 = _mm512_loadu_si512((const void *)(s + i * 64));
    __m512i v1 = _mm512_loadu_si512((const void *)(s + i * 64 + 64));
    _mm512_stream_si512((void *)(d + i * 64), v0);
    _mm512_stream_si512((void *)(d + i * 64 + 64), v1);
  }
  for (; i < blocks; ++i) {
    __m512i val = _mm512_loadu_si512((const void *)(s + i * 64));
    _mm512_stream_si512((void *)(d + i * 64), val);
  }
}

__attribute__((target("avx2")))
#endif
static void rbipc_stream_copy_avx2_blocks(uint8_t *RBIPC_RESTRICT d,
                                          const uint8_t *RBIPC_RESTRICT s,
                                          size_t blocks) {
  size_t i = 0;
  /* Unroll by 4 (128 bytes = 2 full 64-byte write-combining cache lines) */
  for (; i + 4 <= blocks; i += 4) {
    __m256i v0 = _mm256_loadu_si256((const __m256i *)(s + i * 32));
    __m256i v1 = _mm256_loadu_si256((const __m256i *)(s + i * 32 + 32));
    __m256i v2 = _mm256_loadu_si256((const __m256i *)(s + i * 32 + 64));
    __m256i v3 = _mm256_loadu_si256((const __m256i *)(s + i * 32 + 96));
    _mm256_stream_si256((__m256i *)(d + i * 32), v0);
    _mm256_stream_si256((__m256i *)(d + i * 32 + 32), v1);
    _mm256_stream_si256((__m256i *)(d + i * 32 + 64), v2);
    _mm256_stream_si256((__m256i *)(d + i * 32 + 96), v3);
  }
  for (; i < blocks; ++i) {
    __m256i val = _mm256_loadu_si256((const __m256i *)(s + i * 32));
    _mm256_stream_si256((__m256i *)(d + i * 32), val);
  }
}

static void rbipc_stream_copy_sse2_blocks(uint8_t *RBIPC_RESTRICT d,
                                          const uint8_t *RBIPC_RESTRICT s,
                                          size_t blocks) {
  size_t i = 0;
  /* Unroll by 4 (64 bytes = 1 full 64-byte write-combining cache line) */
  for (; i + 4 <= blocks; i += 4) {
    __m128i v0 = _mm_loadu_si128((const __m128i *)(s + i * 16));
    __m128i v1 = _mm_loadu_si128((const __m128i *)(s + i * 16 + 16));
    __m128i v2 = _mm_loadu_si128((const __m128i *)(s + i * 16 + 32));
    __m128i v3 = _mm_loadu_si128((const __m128i *)(s + i * 16 + 48));
    _mm_stream_si128((__m128i *)(d + i * 16), v0);
    _mm_stream_si128((__m128i *)(d + i * 16 + 16), v1);
    _mm_stream_si128((__m128i *)(d + i * 16 + 32), v2);
    _mm_stream_si128((__m128i *)(d + i * 16 + 48), v3);
  }
  for (; i < blocks; ++i) {
    __m128i val = _mm_loadu_si128((const __m128i *)(s + i * 16));
    _mm_stream_si128((__m128i *)(d + i * 16), val);
  }
}
#elif defined(__aarch64__)

static void rbipc_stream_copy_arm64_blocks(uint8_t *RBIPC_RESTRICT d,
                                           const uint8_t *RBIPC_RESTRICT s,
                                           size_t blocks) {
  for (size_t i = 0; i < blocks; ++i) {
    __asm__ __volatile__(
        "ldp q0, q1, [%[src]]\n"
        "ldp q2, q3, [%[src], #32]\n"
        "stnp q0, q1, [%[dst]]\n"
        "stnp q2, q3, [%[dst], #32]\n"
        :
        : [dst] "r"(d + i * 64), [src] "r"(s + i * 64)
        : "q0", "q1", "q2", "q3", "memory");
  }
}
#endif

RBIPC_LEAF

RBIPC_LEAF size_t rbipc_stream_threshold(void) {
  const rbipc_hardware_profile_t *prof = rbipc_get_hardware_profile();
  return (prof && prof->stream_threshold > 0) ? prof->stream_threshold : RBIPC_STREAM_THRESHOLD_DEFAULT;
}

int rbipc_stream_copy(void *RBIPC_RESTRICT dst, const void *RBIPC_RESTRICT src,
                      size_t size) {
  if (RBIPC_UNLIKELY(!dst || !src)) {
    return RBIPC_ERR_INVAL;
  }
  uint8_t *d = (uint8_t *)dst;
  const uint8_t *s = (const uint8_t *)src;

#if defined(__x86_64__) || defined(_M_X64)
  size_t stream_thresh = rbipc_stream_threshold();
  if (size >= stream_thresh) {
    uintptr_t d_addr = (uintptr_t)d;
    size_t align = RBIPC_CACHE_LINE;
    /*
     * Microarchitectural Optimization: Two's Complement Boundary Alignment.
     *
     * In two's complement arithmetic, for any power-of-two alignment boundary A,
     * the number of bytes required to advance an address d to the next multiple of A
     * is given by the algebraic identity:
     * \f[
     *   \text{align\_offset} = (-d) \ \& \ (A - 1)
     * \f]
     * This replaces a multi-instruction dependency chain `(A - (d & (A - 1))) & (A - 1)`
     * with a single negation (`neg`) and bitwise AND (`and`), reducing register pressure
     * and ALU latency before streaming memory operations.
     *
     * Reference:
     * - Henry S. Warren, Jr., "Hacker's Delight" (2nd ed., Addison-Wesley), §3.1.
     */
    size_t align_offset = (-d_addr) & (align - 1U);
    if (align_offset > 0 && align_offset <= size) {
      memcpy(d, s, align_offset);
      d += align_offset;
      s += align_offset;
      size -= align_offset;
    }

#if defined(__GNUC__) || defined(__clang__)
    if (__builtin_cpu_supports("avx512f") && size >= 64) {
      size_t blocks = size / 64;
      rbipc_stream_copy_avx512_blocks(d, s, blocks);
      d += blocks * 64;
      s += blocks * 64;
      size %= 64;
      _mm_sfence();
    } else if (__builtin_cpu_supports("avx2") && size >= 32) {
      size_t blocks = size / 32;
      rbipc_stream_copy_avx2_blocks(d, s, blocks);
      d += blocks * 32;
      s += blocks * 32;
      size %= 32;
      _mm_sfence();
    } else
#endif
        if (size >= 16) {
      size_t blocks = size / 16;
      rbipc_stream_copy_sse2_blocks(d, s, blocks);
      d += blocks * 16;
      s += blocks * 16;
      size %= 16;
      _mm_sfence();
    }
  }
#elif defined(__aarch64__)
  if (size >= RBIPC_STREAM_THRESHOLD) {
    uintptr_t d_addr = (uintptr_t)d;
    size_t align_offset = (-d_addr) & 15U;
    if (align_offset > 0 && align_offset <= size) {
      memcpy(d, s, align_offset);
      d += align_offset;
      s += align_offset;
      size -= align_offset;
    }
    if (size >= 64) {
      size_t blocks = size / 64;
      rbipc_stream_copy_arm64_blocks(d, s, blocks);
      d += blocks * 64;
      s += blocks * 64;
      size %= 64;
      __asm__ __volatile__("dmb ishst" ::: "memory");
    }
  }
#endif

  if (size > 0) {
    memcpy(d, s, size);
  }
  return RBIPC_OK;
}

RBIPC_LEAF
int rbipc_write_stream_timeout(rbipc_ring_t *RBIPC_RESTRICT ring,
                               const void *RBIPC_RESTRICT src, uint32_t len,
                               uint64_t timeout_ns,
                               uint64_t *RBIPC_RESTRICT out_ticket) {
  if (RBIPC_UNLIKELY(!ring || !src || !out_ticket)) {
    return RBIPC_ERR_INVAL;
  }
  void *buf = NULL;
  int rc = rbipc_reserve_write_timeout(ring, len, timeout_ns, &buf, out_ticket);
  if (RBIPC_UNLIKELY(rc != RBIPC_OK)) {
    return rc;
  }
  int copy_rc = rbipc_stream_copy(buf, src, len);
  if (RBIPC_UNLIKELY(copy_rc != RBIPC_OK)) {
    rbipc_abort_write(ring, *out_ticket);
    return copy_rc;
  }
  return rbipc_commit_write(ring, *out_ticket, len);
}

RBIPC_LEAF
int rbipc_write_stream(rbipc_ring_t *RBIPC_RESTRICT ring,
                       const void *RBIPC_RESTRICT src, uint32_t len,
                       uint64_t *RBIPC_RESTRICT out_ticket) {
  return rbipc_write_stream_timeout(ring, src, len, UINT64_MAX, out_ticket);
}

RBIPC_LEAF
int rbipc_write_stream_nonblock(rbipc_ring_t *RBIPC_RESTRICT ring,
                                const void *RBIPC_RESTRICT src, uint32_t len,
                                uint64_t *RBIPC_RESTRICT out_ticket) {
  return rbipc_write_stream_timeout(ring, src, len, 0, out_ticket);
}
