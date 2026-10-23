/**
 * @file rbipc_ipc.c
 * @brief Implementation of UNIX domain socket SCM_RIGHTS descriptor transmission.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "rbipc_ipc.h"
#include "rbipc_predicate.h"

#include <sys/socket.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

/**
 * @brief Transmit multiple file descriptors across a UNIX domain socket via SCM_RIGHTS.
 */
RBIPC_LEAF
int rbipc_ipc_send_fds(int socket_fd, const int *fds, size_t count) {
    if (RBIPC_UNLIKELY(!rbipc_is_valid_fd(socket_fd) || !fds || count == 0 || count > RBIPC_IPC_MAX_FDS)) {
        return RBIPC_ERR_INVAL;
    }
    for (size_t i = 0; i < count; ++i) {
        if (RBIPC_UNLIKELY(!rbipc_is_valid_fd(fds[i]))) {
            return RBIPC_ERR_INVAL;
        }
    }

    struct msghdr msg = {};
    char dummy = 'R';
    struct iovec iov = {
        .iov_base = &dummy,
        .iov_len = sizeof(dummy)
    };
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    char cmsg_buf[CMSG_SPACE(RBIPC_IPC_MAX_FDS * sizeof(int))] = {};
    msg.msg_control = cmsg_buf;
    msg.msg_controllen = CMSG_SPACE(count * sizeof(int));

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (RBIPC_UNLIKELY(!cmsg)) {
        return RBIPC_ERR_SYS;
    }
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(count * sizeof(int));
    memcpy(CMSG_DATA(cmsg), fds, count * sizeof(int));

    ssize_t ret = sendmsg(socket_fd, &msg, 0);
    if (RBIPC_UNLIKELY(ret <= 0)) {
        return RBIPC_ERR_SYS;
    }

    return RBIPC_OK;
}

/**
 * @brief Transmit a raw file descriptor across a UNIX domain socket via SCM_RIGHTS.
 */
RBIPC_LEAF
int rbipc_ipc_send_fd(int socket_fd, int fd_to_send) {
    return rbipc_ipc_send_fds(socket_fd, &fd_to_send, 1);
}

/**
 * @brief Receive multiple file descriptors from a UNIX domain socket via SCM_RIGHTS.
 */
RBIPC_LEAF
int rbipc_ipc_recv_fds(int socket_fd, int *out_fds, size_t max_count, size_t *out_count) {
    if (RBIPC_UNLIKELY(!rbipc_is_valid_fd(socket_fd) || !out_fds || max_count == 0 || !out_count)) {
        return RBIPC_ERR_INVAL;
    }
    *out_count = 0;
    for (size_t i = 0; i < max_count; ++i) {
        out_fds[i] = -1;
    }

    struct msghdr msg = {};
    char dummy = 0;
    struct iovec iov = {
        .iov_base = &dummy,
        .iov_len = sizeof(dummy)
    };
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    char cmsg_buf[CMSG_SPACE(RBIPC_IPC_MAX_FDS * sizeof(int))] = {};
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

    size_t data_bytes = (cmsg->cmsg_len > sizeof(struct cmsghdr)) ? (cmsg->cmsg_len - sizeof(struct cmsghdr)) : 0;
    size_t num_fds = data_bytes / sizeof(int);
    if (RBIPC_UNLIKELY(num_fds == 0)) {
        return RBIPC_ERR_SYS;
    }

    size_t copy_count = (num_fds < max_count) ? num_fds : max_count;
    memcpy(out_fds, CMSG_DATA(cmsg), copy_count * sizeof(int));

    /* Safely close any excess descriptors beyond caller's max_count */
    if (RBIPC_UNLIKELY(num_fds > max_count)) {
        int excess_fds[RBIPC_IPC_MAX_FDS];
        size_t excess_count = num_fds - max_count;
        if (excess_count > RBIPC_IPC_MAX_FDS) excess_count = RBIPC_IPC_MAX_FDS;
        memcpy(excess_fds, ((const char *)CMSG_DATA(cmsg)) + max_count * sizeof(int),
               excess_count * sizeof(int));
        for (size_t i = 0; i < excess_count; ++i) {
            if (rbipc_is_valid_fd(excess_fds[i])) {
                close(excess_fds[i]);
            }
        }
    }

    *out_count = copy_count;
    return RBIPC_OK;
}

/**
 * @brief Receive a raw file descriptor from a UNIX domain socket via SCM_RIGHTS.
 */
RBIPC_LEAF
int rbipc_ipc_recv_fd(int socket_fd, int *out_received_fd) {
    if (RBIPC_UNLIKELY(!out_received_fd)) {
        return RBIPC_ERR_INVAL;
    }
    size_t received_count = 0;
    int rc = rbipc_ipc_recv_fds(socket_fd, out_received_fd, 1, &received_count);
    if (RBIPC_UNLIKELY(rc != RBIPC_OK || received_count == 0)) {
        return (rc != RBIPC_OK) ? rc : RBIPC_ERR_SYS;
    }
    return RBIPC_OK;
}

/**
 * @brief Transmit the backing file descriptor (and optional eventfd reactor) of a ring buffer across a UNIX domain socket.
 */
RBIPC_LEAF
int rbipc_send_fd(int socket_fd, const rbipc_ring_t *ring) {
    if (RBIPC_UNLIKELY(!rbipc_is_valid_fd(socket_fd) || !ring)) {
        return RBIPC_ERR_INVAL;
    }
    int shm_fd = rbipc_get_fd(ring);
    if (RBIPC_UNLIKELY(!rbipc_is_valid_fd(shm_fd))) {
        return RBIPC_ERR_INVAL;
    }
    int event_fd = -1;
    if (rbipc_get_eventfd(ring, &event_fd) == RBIPC_OK && rbipc_is_valid_fd(event_fd)) {
        int fds[2] = { shm_fd, event_fd };
        return rbipc_ipc_send_fds(socket_fd, fds, 2);
    }
    return rbipc_ipc_send_fds(socket_fd, &shm_fd, 1);
}

/**
 * @brief Receive a ring buffer descriptor across a socket and attach with role flags, auto-binding eventfd if present.
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

    int received_fds[2] = { -1, -1 };
    size_t received_count = 0;
    int rc = rbipc_ipc_recv_fds(socket_fd, received_fds, 2, &received_count);
    if (RBIPC_UNLIKELY(rc != RBIPC_OK || received_count == 0)) {
        return (rc != RBIPC_OK) ? rc : RBIPC_ERR_SYS;
    }

    rc = rbipc_attach_fd_ex(received_fds[0], role_flags, out_ring);
    if (RBIPC_UNLIKELY(rc != RBIPC_OK)) {
        close(received_fds[0]);
        if (received_count > 1 && rbipc_is_valid_fd(received_fds[1])) {
            close(received_fds[1]);
        }
        return rc;
    }

    if (received_count > 1 && rbipc_is_valid_fd(received_fds[1])) {
        int s_rc = rbipc_set_eventfd(*out_ring, received_fds[1]);
        if (RBIPC_UNLIKELY(s_rc != RBIPC_OK)) {
            close(received_fds[1]);
        }
    }

    return RBIPC_OK;
}

/**
 * @brief Receive a ring buffer descriptor across a socket and attach as producer and consumer.
 */
RBIPC_LEAF
int rbipc_recv_fd(int socket_fd, rbipc_ring_t **out_ring) {
    return rbipc_recv_fd_ex(socket_fd, RBIPC_ROLE_PRODUCER | RBIPC_ROLE_CONSUMER, out_ring);
}

RBIPC_LEAF
int rbipc_ipc_get_peer_cred(int socket_fd, rbipc_peer_cred_t *out_cred) {
    if (RBIPC_UNLIKELY(!rbipc_is_valid_fd(socket_fd) || !out_cred)) {
        return RBIPC_ERR_INVAL;
    }
#ifdef SO_PEERCRED
    struct ucred cred = {};
    socklen_t len = sizeof(cred);
    if (getsockopt(socket_fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) {
        return RBIPC_ERR_SYS;
    }
    out_cred->pid = cred.pid;
    out_cred->uid = cred.uid;
    out_cred->gid = cred.gid;
    return RBIPC_OK;
#else
    return RBIPC_ERR_NOTSUP;
#endif
}

RBIPC_LEAF
int rbipc_ipc_verify_peer(int socket_fd, uid_t expected_uid, gid_t expected_gid) {
    rbipc_peer_cred_t cred;
    int rc = rbipc_ipc_get_peer_cred(socket_fd, &cred);
    if (RBIPC_UNLIKELY(rc != RBIPC_OK)) {
        return rc;
    }
    if (expected_uid != (uid_t)-1 && cred.uid != expected_uid) {
        return RBIPC_ERR_INVAL;
    }
    if (expected_gid != (gid_t)-1 && cred.gid != expected_gid) {
        return RBIPC_ERR_INVAL;
    }
    return RBIPC_OK;
}

RBIPC_LEAF
int rbipc_send_fd_auth(int socket_fd, const rbipc_ring_t *ring, uid_t expected_uid,
                       gid_t expected_gid) {
    int rc = rbipc_ipc_verify_peer(socket_fd, expected_uid, expected_gid);
    if (RBIPC_UNLIKELY(rc != RBIPC_OK)) {
        return rc;
    }
    return rbipc_send_fd(socket_fd, ring);
}

RBIPC_LEAF
int rbipc_recv_fd_auth(int socket_fd, uint32_t role_flags, uid_t expected_uid,
                       gid_t expected_gid, rbipc_ring_t **out_ring) {
    int rc = rbipc_ipc_verify_peer(socket_fd, expected_uid, expected_gid);
    if (RBIPC_UNLIKELY(rc != RBIPC_OK)) {
        return rc;
    }
    return rbipc_recv_fd_ex(socket_fd, role_flags, out_ring);
}
