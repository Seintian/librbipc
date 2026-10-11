/**
 * @file rbipc_ipc.c
 * @brief Implementation of UNIX domain socket SCM_RIGHTS descriptor transmission.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "rbipc_ipc.h"
#include "rbipc_internal.h"
#include "rbipc_predicate.h"

#include <sys/socket.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

/**
 * @brief Transmit a raw file descriptor across a UNIX domain socket via SCM_RIGHTS.
 *
 * @param[in] socket_fd  Connected UNIX domain socket descriptor.
 * @param[in] fd_to_send File descriptor to transmit.
 *
 * @return RBIPC_OK on success, or RBIPC_ERR_INVAL / RBIPC_ERR_SYS on failure.
 */
RBIPC_LEAF
int rbipc_ipc_send_fd(int socket_fd, int fd_to_send) {
    if (RBIPC_UNLIKELY(!rbipc_is_valid_fd(socket_fd) || !rbipc_is_valid_fd(fd_to_send))) {
        return RBIPC_ERR_INVAL;
    }

    struct msghdr msg = {0};
    char dummy = 'R';
    struct iovec iov = {
        .iov_base = &dummy,
        .iov_len = sizeof(dummy)
    };
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    char cmsg_buf[CMSG_SPACE(sizeof(int))];
    memset(cmsg_buf, 0, sizeof(cmsg_buf));
    msg.msg_control = cmsg_buf;
    msg.msg_controllen = sizeof(cmsg_buf);

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (RBIPC_UNLIKELY(!cmsg)) {
        return RBIPC_ERR_SYS;
    }
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &fd_to_send, sizeof(int));

    ssize_t ret = sendmsg(socket_fd, &msg, 0);
    if (RBIPC_UNLIKELY(ret <= 0)) {
        return RBIPC_ERR_SYS;
    }

    return RBIPC_OK;
}

/**
 * @brief Receive a raw file descriptor from a UNIX domain socket via SCM_RIGHTS.
 *
 * @param[in]  socket_fd       Connected UNIX domain socket descriptor.
 * @param[out] out_received_fd Location to store received file descriptor.
 *
 * @return RBIPC_OK on success, or error code on failure.
 */
RBIPC_LEAF
int rbipc_ipc_recv_fd(int socket_fd, int *out_received_fd) {
    if (RBIPC_UNLIKELY(!rbipc_is_valid_fd(socket_fd) || !out_received_fd)) {
        return RBIPC_ERR_INVAL;
    }
    *out_received_fd = -1;

    struct msghdr msg = {0};
    char dummy = 0;
    struct iovec iov = {
        .iov_base = &dummy,
        .iov_len = sizeof(dummy)
    };
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    char cmsg_buf[CMSG_SPACE(sizeof(int))];
    memset(cmsg_buf, 0, sizeof(cmsg_buf));
    msg.msg_control = cmsg_buf;
    msg.msg_controllen = sizeof(cmsg_buf);

    ssize_t ret = recvmsg(socket_fd, &msg, 0);
    if (RBIPC_UNLIKELY(ret <= 0)) {
        return RBIPC_ERR_SYS;
    }

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (RBIPC_UNLIKELY(!cmsg || cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS)) {
        return RBIPC_ERR_SYS;
    }
    if (RBIPC_UNLIKELY(cmsg->cmsg_len < CMSG_LEN(sizeof(int)))) {
        return RBIPC_ERR_SYS;
    }

    int received_fd = -1;
    memcpy(&received_fd, CMSG_DATA(cmsg), sizeof(int));
    if (RBIPC_UNLIKELY(!rbipc_is_valid_fd(received_fd))) {
        return RBIPC_ERR_SYS;
    }

    *out_received_fd = received_fd;
    return RBIPC_OK;
}

/**
 * @brief Transmit the backing file descriptor of a ring buffer across a UNIX domain socket.
 *
 * @param[in] socket_fd Connected UNIX domain socket descriptor.
 * @param[in] ring      Ring buffer handle whose backing descriptor will be sent.
 *
 * @return RBIPC_OK on success, or error code on failure.
 */
RBIPC_LEAF
int rbipc_send_fd(int socket_fd, const rbipc_ring_t *ring) {
    if (RBIPC_UNLIKELY(!rbipc_is_valid_fd(socket_fd) || !ring)) {
        return RBIPC_ERR_INVAL;
    }
    int fd = rbipc_get_fd(ring);
    if (RBIPC_UNLIKELY(!rbipc_is_valid_fd(fd))) {
        return RBIPC_ERR_INVAL;
    }
    return rbipc_ipc_send_fd(socket_fd, fd);
}

/**
 * @brief Receive a ring buffer descriptor across a socket and attach with role flags.
 *
 * @param[in]  socket_fd  Connected UNIX domain socket descriptor.
 * @param[in]  role_flags Participant role flags.
 * @param[out] out_ring   Location to store attached ring buffer handle.
 *
 * @return RBIPC_OK on success, or error code on failure.
 */
RBIPC_LEAF
int rbipc_recv_fd_ex(int socket_fd, uint32_t role_flags, rbipc_ring_t **out_ring) {
    if (RBIPC_UNLIKELY(!rbipc_is_valid_fd(socket_fd) || !out_ring)) {
        return RBIPC_ERR_INVAL;
    }
    *out_ring = NULL;

    uint32_t known_roles = RBIPC_ROLE_PRODUCER | RBIPC_ROLE_CONSUMER | RBIPC_ROLE_MONITOR;
    if (RBIPC_UNLIKELY(role_flags == 0 || (role_flags & ~known_roles) != 0)) {
        return RBIPC_ERR_INVAL;
    }

    int received_fd = -1;
    int rc = rbipc_ipc_recv_fd(socket_fd, &received_fd);
    if (RBIPC_UNLIKELY(rc != RBIPC_OK)) {
        return rc;
    }

    rc = rbipc_attach_fd_ex(received_fd, role_flags, out_ring);
    if (RBIPC_UNLIKELY(rc != RBIPC_OK)) {
        close(received_fd);
        return rc;
    }

    return RBIPC_OK;
}

/**
 * @brief Receive a ring buffer descriptor across a socket and attach as producer and consumer.
 *
 * @param[in]  socket_fd Connected UNIX domain socket descriptor.
 * @param[out] out_ring  Location to store attached ring buffer handle.
 *
 * @return RBIPC_OK on success, or error code on failure.
 */
RBIPC_LEAF
int rbipc_recv_fd(int socket_fd, rbipc_ring_t **out_ring) {
    return rbipc_recv_fd_ex(socket_fd, RBIPC_ROLE_PRODUCER | RBIPC_ROLE_CONSUMER, out_ring);
}
