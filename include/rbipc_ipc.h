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
 * @def RBIPC_IPC_MAX_FDS
 * @brief Maximum count of file descriptors that can be transmitted in a single SCM_RIGHTS message (8).
 */
#define RBIPC_IPC_MAX_FDS 253U /**< Maximum fds transmissible via SCM_RIGHTS (Linux SCM_MAX_FD). */

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
 * @brief Transmit multiple file descriptors across a UNIX domain socket via SCM_RIGHTS.
 *
 * @param[in] socket_fd An open, connected UNIX domain socket (AF_UNIX).
 * @param[in] fds       Array of open file descriptors to transmit.
 * @param[in] count     Number of file descriptors in @p fds array (must be > 0 and <= @ref RBIPC_IPC_MAX_FDS).
 *
 * @return @ref RBIPC_OK on success.
 * @retval RBIPC_ERR_INVAL if arguments are invalid or any fd is negative.
 * @retval RBIPC_ERR_SYS   if sendmsg fails.
 */
RBIPC_API RBIPC_NODISCARD RBIPC_LEAF int
rbipc_ipc_send_fds(int socket_fd, const int *fds, size_t count);

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
 * @brief Receive multiple file descriptors from a UNIX domain socket via SCM_RIGHTS.
 *
 * @param[in]  socket_fd An open, connected UNIX domain socket (AF_UNIX).
 * @param[out] out_fds   Array to store received file descriptors.
 * @param[in]  max_count Maximum number of file descriptors that can be stored in @p out_fds.
 * @param[out] out_count Pointer to store actual count of received file descriptors.
 *
 * @return @ref RBIPC_OK on success.
 * @retval RBIPC_ERR_INVAL if arguments are invalid or max_count is 0.
 * @retval RBIPC_ERR_SYS   if recvmsg fails or ancillary SCM_RIGHTS payload is missing.
 */
RBIPC_API RBIPC_NODISCARD RBIPC_LEAF int
rbipc_ipc_recv_fds(int socket_fd, int *out_fds, size_t max_count, size_t *out_count);

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

/**
 * @struct rbipc_peer_cred_t
 * @brief Kernel-authenticated credentials of a connected UNIX domain socket peer (SO_PEERCRED).
 */
typedef struct {
  pid_t pid; /**< Process ID of the remote peer. */
  uid_t uid; /**< Real User ID of the remote peer. */
  gid_t gid; /**< Real Group ID of the remote peer. */
} rbipc_peer_cred_t;

/**
 * @brief Retrieve kernel-authenticated credentials of a connected UNIX domain socket peer.
 *
 * @param[in]  socket_fd An open, connected UNIX domain socket (AF_UNIX).
 * @param[out] out_cred  Location to store authenticated peer credentials.
 *
 * @return @ref RBIPC_OK on success.
 * @retval RBIPC_ERR_INVAL if @p socket_fd is negative or @p out_cred is NULL.
 * @retval RBIPC_ERR_SYS   if getsockopt fails.
 */
RBIPC_API RBIPC_NODISCARD RBIPC_LEAF int
rbipc_ipc_get_peer_cred(int socket_fd, rbipc_peer_cred_t *out_cred);

/**
 * @brief Verify that the UNIX domain socket peer matches expected UID and GID before descriptor transfer.
 *
 * @param[in] socket_fd    An open, connected UNIX domain socket (AF_UNIX).
 * @param[in] expected_uid Expected User ID (or (uid_t)-1 to ignore UID check).
 * @param[in] expected_gid Expected Group ID (or (gid_t)-1 to ignore GID check).
 *
 * @return @ref RBIPC_OK if peer matches expected credentials.
 * @retval RBIPC_ERR_INVAL if credentials do not match or arguments are invalid.
 * @retval RBIPC_ERR_SYS   if getsockopt fails.
 */
RBIPC_API RBIPC_NODISCARD RBIPC_LEAF int
rbipc_ipc_verify_peer(int socket_fd, uid_t expected_uid, gid_t expected_gid);

/**
 * @brief Authenticate the connected peer and transmit the ring buffer descriptor across a UNIX domain socket.
 *
 * @param[in] socket_fd    An open, connected UNIX domain socket (AF_UNIX).
 * @param[in] ring         Ring buffer instance whose backing fd will be passed.
 * @param[in] expected_uid Expected peer User ID (or (uid_t)-1 to ignore).
 * @param[in] expected_gid Expected peer Group ID (or (gid_t)-1 to ignore).
 *
 * @return @ref RBIPC_OK on success.
 * @retval RBIPC_ERR_INVAL if verification fails or parameters are invalid.
 * @retval RBIPC_ERR_SYS   if system calls fail.
 */
RBIPC_API RBIPC_NODISCARD RBIPC_LEAF int
rbipc_send_fd_auth(int socket_fd, const rbipc_ring_t *ring, uid_t expected_uid,
                   gid_t expected_gid);

/**
 * @brief Authenticate the sending peer and receive a ring buffer descriptor across a UNIX domain socket.
 *
 * @param[in]  socket_fd    An open, connected UNIX domain socket (AF_UNIX).
 * @param[in]  role_flags   Bitmask of role flags (RBIPC_ROLE_PRODUCER, etc.).
 * @param[in]  expected_uid Expected peer User ID (or (uid_t)-1 to ignore).
 * @param[in]  expected_gid Expected peer Group ID (or (gid_t)-1 to ignore).
 * @param[out] out_ring     Location to store the attached ring buffer handle.
 *
 * @return @ref RBIPC_OK on success.
 * @retval RBIPC_ERR_INVAL if verification fails or parameters are invalid.
 * @retval RBIPC_ERR_SYS   if system calls fail.
 */
RBIPC_API RBIPC_NODISCARD RBIPC_LEAF int
rbipc_recv_fd_auth(int socket_fd, uint32_t role_flags, uid_t expected_uid,
                   gid_t expected_gid, rbipc_ring_t **out_ring);

#ifdef __cplusplus
}
#endif

#endif /* RBIPC_IPC_H */
