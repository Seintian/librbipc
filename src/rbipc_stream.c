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
#include "rbipc_io.h"
#include "rbipc_predicate.h"

#include <string.h>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>

#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("avx2")))
#endif
static void rbipc_stream_copy_avx2_blocks(uint8_t *RBIPC_RESTRICT d,
                                          const uint8_t *RBIPC_RESTRICT s,
                                          size_t blocks) {
  for (size_t i = 0; i < blocks; ++i) {
    __m256i val = _mm256_loadu_si256((const __m256i *)(s + i * 32));
    _mm256_stream_si256((__m256i *)(d + i * 32), val);
  }
}

static void rbipc_stream_copy_sse2_blocks(uint8_t *RBIPC_RESTRICT d,
                                          const uint8_t *RBIPC_RESTRICT s,
                                          size_t blocks) {
  for (size_t i = 0; i < blocks; ++i) {
    __m128i val = _mm_loadu_si128((const __m128i *)(s + i * 16));
    _mm_stream_si128((__m128i *)(d + i * 16), val);
  }
}
#endif

RBIPC_LEAF
int rbipc_stream_copy(void *RBIPC_RESTRICT dst, const void *RBIPC_RESTRICT src,
                      size_t size) {
  if (RBIPC_UNLIKELY(!dst || !src)) {
    return RBIPC_ERR_INVAL;
  }
  uint8_t *d = (uint8_t *)dst;
  const uint8_t *s = (const uint8_t *)src;

#if defined(__x86_64__) || defined(_M_X64)
  if (size >= RBIPC_STREAM_THRESHOLD) {
    uintptr_t d_addr = (uintptr_t)d;
    size_t align_offset = (32U - (d_addr & 31U)) & 31U;
    if (align_offset > 0 && align_offset <= size) {
      memcpy(d, s, align_offset);
      d += align_offset;
      s += align_offset;
      size -= align_offset;
    }

#if defined(__GNUC__) || defined(__clang__)
    if (__builtin_cpu_supports("avx2") && size >= 32) {
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