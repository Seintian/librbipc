/**
 * @file rbipc_reactor.h
 * @brief Asynchronous eventfd and epoll reactor notification engine.
 *
 * @details Implements non-blocking eventfd descriptor allocation, binding,
 * notification dispatching, and counter draining for integration into event
 * loop reactors (epoll, io_uring).
 *
 * References:
 * - eventfd(2) / epoll(7) asynchronous edge/level triggered readiness
 * multiplexing.
 *
 * @author Seintian
 * @date 2026
 * @copyright Apache License 2.0
 */

#ifndef RBIPC_REACTOR_H
#define RBIPC_REACTOR_H

#include "rbipc.h"
#include "rbipc_attr.h"

/**
 * @brief Enable asynchronous eventfd signaling on the ring buffer instance.
 *
 * @param[in,out] ring   Ring buffer handle.
 * @param[out]    out_fd Location to store the created eventfd descriptor.
 *
 * @return RBIPC_OK on success, RBIPC_ERR_INVAL if parameters invalid,
 * RBIPC_ERR_SYS on syscall failure.
 */
RBIPC_INTERNAL RBIPC_NODISCARD RBIPC_LEAF int
rbipc_reactor_enable(rbipc_ring_t *RBIPC_RESTRICT ring,
                     int *RBIPC_RESTRICT out_fd);

/**
 * @brief Bind an external eventfd file descriptor to the ring buffer instance.
 *
 * @param[in,out] ring     Ring buffer handle.
 * @param[in]     event_fd Pre-created eventfd descriptor to bind.
 *
 * @return RBIPC_OK on success, or RBIPC_ERR_INVAL if arguments invalid.
 */
RBIPC_INTERNAL RBIPC_NODISCARD RBIPC_LEAF int
rbipc_reactor_set_fd(rbipc_ring_t *RBIPC_RESTRICT ring, int event_fd);

/**
 * @brief Query currently bound eventfd file descriptor from ring buffer
 * instance.
 *
 * @param[in]  ring   Ring buffer handle.
 * @param[out] out_fd Location to store the bound descriptor.
 *
 * @return RBIPC_OK on success, RBIPC_ERR_NOTSUP if no eventfd is enabled.
 */
RBIPC_INTERNAL RBIPC_NODISCARD RBIPC_LEAF int
rbipc_reactor_get_fd(const rbipc_ring_t *RBIPC_RESTRICT ring,
                     int *RBIPC_RESTRICT out_fd);

/**
 * @brief Drain accumulated eventfd counter to reset readiness state.
 *
 * @param[in] ring Ring buffer handle.
 *
 * @return RBIPC_OK on success, RBIPC_ERR_NOTSUP if disabled, RBIPC_ERR_SYS on
 * failure.
 */
RBIPC_INTERNAL RBIPC_LEAF
int rbipc_reactor_drain(const rbipc_ring_t *RBIPC_RESTRICT ring);

/**
 * @brief Dispatch an asynchronous readiness signal via the bound eventfd if
 * reactor waiters are present.
 *
 * @param[in] ring Ring buffer handle.
 */
RBIPC_INTERNAL RBIPC_LEAF
void rbipc_reactor_signal(const rbipc_ring_t *ring);

/**
 * @brief Tear down and clean up reactor descriptors associated with a ring
 * handle on detach.
 *
 * @param[in,out] ring Ring buffer handle being detached.
 */
RBIPC_INTERNAL RBIPC_LEAF
void rbipc_reactor_cleanup(rbipc_ring_t *ring);

#endif /* RBIPC_REACTOR_H */
