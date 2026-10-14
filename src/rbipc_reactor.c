/**
 * @file rbipc_reactor.c
 * @brief Implementation of asynchronous eventfd and epoll reactor notification
 * engine.
 *
 * @details Implements non-blocking eventfd creation, binding, notification
 * signaling, counter draining, and resource cleanup.
 *
 * @author Seintian
 * @date 2026
 * @copyright Apache License 2.0
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "rbipc_reactor.h"
#include "rbipc_internal.h"

#include <errno.h>
#include <sys/eventfd.h>
#include <unistd.h>

RBIPC_LEAF
int rbipc_reactor_enable(rbipc_ring_t *RBIPC_RESTRICT ring,
                         int *RBIPC_RESTRICT out_fd) {
  if (RBIPC_UNLIKELY(!ring || !out_fd)) {
    return RBIPC_ERR_INVAL;
  }
  if (ring->event_fd >= 0) {
    *out_fd = ring->event_fd;
    return RBIPC_OK;
  }
  int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (RBIPC_UNLIKELY(fd < 0)) {
    return RBIPC_ERR_SYS;
  }
  ring->event_fd = fd;
  ring->owns_eventfd = true;
  if (ring->hdr) {
    atomic_fetch_add_explicit(&ring->hdr->eventfd_waiters, 1,
                              memory_order_release);
  }
  *out_fd = fd;
  return RBIPC_OK;
}

RBIPC_LEAF
int rbipc_reactor_set_fd(rbipc_ring_t *RBIPC_RESTRICT ring, int event_fd) {
  if (RBIPC_UNLIKELY(!ring || event_fd < 0)) {
    return RBIPC_ERR_INVAL;
  }
  if (ring->event_fd >= 0 && ring->owns_eventfd) {
    if (ring->hdr) {
      atomic_fetch_sub_explicit(&ring->hdr->eventfd_waiters, 1,
                                memory_order_release);
    }
    close(ring->event_fd);
  }
  ring->event_fd = event_fd;
  ring->owns_eventfd = false;
  if (ring->hdr) {
    atomic_fetch_add_explicit(&ring->hdr->eventfd_waiters, 1,
                              memory_order_release);
  }
  return RBIPC_OK;
}

RBIPC_LEAF
int rbipc_reactor_get_fd(const rbipc_ring_t *RBIPC_RESTRICT ring,
                         int *RBIPC_RESTRICT out_fd) {
  if (RBIPC_UNLIKELY(!ring || !out_fd)) {
    return RBIPC_ERR_INVAL;
  }
  if (ring->event_fd < 0) {
    return RBIPC_ERR_NOTSUP;
  }
  *out_fd = ring->event_fd;
  return RBIPC_OK;
}

RBIPC_LEAF
int rbipc_reactor_drain(const rbipc_ring_t *RBIPC_RESTRICT ring) {
  if (RBIPC_UNLIKELY(!ring)) {
    return RBIPC_ERR_INVAL;
  }
  if (ring->event_fd < 0) {
    return RBIPC_ERR_NOTSUP;
  }
  uint64_t val = 0;
  ssize_t n = read(ring->event_fd, &val, sizeof(val));
  if (n < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return RBIPC_OK;
    }
    return RBIPC_ERR_SYS;
  }
  return (n == sizeof(val)) ? RBIPC_OK : RBIPC_ERR_SYS;
}

RBIPC_LEAF
void rbipc_reactor_signal(const rbipc_ring_t *ring) {
  if (RBIPC_UNLIKELY(ring && ring->event_fd >= 0)) {
    if (ring->hdr && atomic_load_explicit(&ring->hdr->eventfd_waiters,
                                          memory_order_relaxed) > 0) {
      uint64_t val = 1;
      ssize_t ret = write(ring->event_fd, &val, sizeof(val));
      (void)ret;
    }
  }
}

RBIPC_LEAF
void rbipc_reactor_cleanup(rbipc_ring_t *ring) {
  if (!ring)
    return;
  if (ring->event_fd >= 0) {
    if (ring->hdr) {
      atomic_fetch_sub_explicit(&ring->hdr->eventfd_waiters, 1,
                                memory_order_release);
    }
    if (ring->owns_eventfd) {
      close(ring->event_fd);
    }
    ring->event_fd = -1;
    ring->owns_eventfd = false;
  }
}

/* ============================================================================
 * Public Eventfd API Wrappers
 * ============================================================================
 */

RBIPC_LEAF
int rbipc_enable_eventfd(rbipc_ring_t *RBIPC_RESTRICT ring,
                         int *RBIPC_RESTRICT out_fd) {
  return rbipc_reactor_enable(ring, out_fd);
}

RBIPC_LEAF
int rbipc_set_eventfd(rbipc_ring_t *RBIPC_RESTRICT ring, int event_fd) {
  return rbipc_reactor_set_fd(ring, event_fd);
}

RBIPC_LEAF
int rbipc_get_eventfd(const rbipc_ring_t *RBIPC_RESTRICT ring,
                      int *RBIPC_RESTRICT out_fd) {
  return rbipc_reactor_get_fd(ring, out_fd);
}

RBIPC_LEAF
int rbipc_eventfd_drain(const rbipc_ring_t *RBIPC_RESTRICT ring) {
  return rbipc_reactor_drain(ring);
}