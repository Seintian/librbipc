/**
 * @file rbipc_ipc.h
 * @brief Inter-process socket utilities and SCM_RIGHTS descriptor passing.
 *
 * @details Provides turnkey helpers to transmit and receive shared-memory file
 * descriptors across UNIX domain sockets (AF_UNIX) using SCM_RIGHTS ancillary
 * data. Enables zero-trust, anonymous memfd sharing without exposing paths in
 * /dev/shm.
 *
 * @author Seintian
 * @date 2026
 * @copyright Apache License 2.0
 */

#ifndef RBIPC_IPC_H
#define RBIPC_IPC_H

#include "rbipc.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Transmit a raw file descriptor across a UNIX domain stream or datagram
 * socket.
 *
 * @param[in] socket_fd  An open, connected UNIX domain socket (AF_UNIX).
 * @param[in] fd_to_send File descriptor to transmit.
 *
 * @return @ref RBIPC_OK on success.
 * @retval RBIPC_ERR_INVAL if @p socket_fd or @p fd_to_send is negative.
 * @retval RBIPC_ERR_SYS   if sendmsg fails.
 */
RBIPC_API RBIPC_NODISCARD RBIPC_LEAF int rbipc_ipc_send_fd(int socket_fd, int fd_to_send);

/**
 * @brief Receive a raw file descriptor from a UNIX domain stream or datagram
 * socket.
 *
 * @param[in]  socket_fd        An open, connected UNIX domain socket (AF_UNIX).
 * @param[out] out_received_fd  Location to store the received file descriptor.
 *
 * @return @ref RBIPC_OK on success.
 * @retval RBIPC_ERR_INVAL if @p socket_fd is negative or @p out_received_fd is
 * NULL.
 * @retval RBIPC_ERR_SYS   if recvmsg fails or ancillary SCM_RIGHTS payload is
 * missing.
 */
RBIPC_API RBIPC_NODISCARD RBIPC_LEAF int rbipc_ipc_recv_fd(int socket_fd,
                                                 int *out_received_fd);

/**
 * @brief Transmit the underlying shared memory descriptor of a ring buffer
 * across a UNIX domain socket.
 *
 * @param[in] socket_fd An open, connected UNIX domain socket (AF_UNIX).
 * @param[in] ring      Ring buffer instance whose backing fd will be passed.
 *
 * @return @ref RBIPC_OK on success.
 * @retval RBIPC_ERR_INVAL if @p socket_fd is negative or @p ring is NULL.
 * @retval RBIPC_ERR_SYS   if sendmsg fails.
 */
RBIPC_API RBIPC_NODISCARD RBIPC_LEAF int rbipc_send_fd(int socket_fd,
                                             const rbipc_ring_t *ring);

/**
 * @brief Receive a ring buffer file descriptor across a UNIX domain socket with
 * specified role flags.
 *
 * @param[in]  socket_fd   An open, connected UNIX domain socket (AF_UNIX).
 * @param[in]  role_flags  Bitmask of role flags (RBIPC_ROLE_PRODUCER,
 * RBIPC_ROLE_CONSUMER, RBIPC_ROLE_MONITOR).
 * @param[out] out_ring    Location to store the attached ring buffer handle.
 *
 * @return @ref RBIPC_OK on success.
 * @retval RBIPC_ERR_INVAL if @p socket_fd is negative, @p role_flags is
 * invalid, or @p out_ring is NULL.
 * @retval RBIPC_ERR_SYS   if recvmsg fails or attachment fails.
 */
RBIPC_API RBIPC_NODISCARD RBIPC_LEAF int
rbipc_recv_fd_ex(int socket_fd, uint32_t role_flags, rbipc_ring_t **out_ring);

/**
 * @brief Receive a ring buffer file descriptor across a UNIX domain socket and
 * attach as standard producer/consumer.
 *
 * @param[in]  socket_fd An open, connected UNIX domain socket (AF_UNIX).
 * @param[out] out_ring  Location to store the attached ring buffer handle.
 *
 * @return @ref RBIPC_OK on success.
 * @retval RBIPC_ERR_INVAL if @p socket_fd is negative or @p out_ring is NULL.
 * @retval RBIPC_ERR_SYS   if recvmsg fails or attachment fails.
 */
RBIPC_API RBIPC_NODISCARD RBIPC_LEAF int rbipc_recv_fd(int socket_fd,
                                             rbipc_ring_t **out_ring);

#ifdef __cplusplus
}
#endif

#endif /* RBIPC_IPC_H */
