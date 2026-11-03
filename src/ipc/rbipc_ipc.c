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

    char stack_cmsg[CMSG_SPACE(8 * sizeof(int))] = {};
    char *cmsg_buf = stack_cmsg;
    size_t control_len = CMSG_SPACE(count * sizeof(int));
    if (count > 8) {
        cmsg_buf = (char *)malloc(control_len);
        if (RBIPC_UNLIKELY(!cmsg_buf)) {
            return RBIPC_ERR_NOMEM;
        }
        memset(cmsg_buf, 0, control_len);
    }
    msg.msg_control = cmsg_buf;
    msg.msg_controllen = control_len;

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (RBIPC_UNLIKELY(!cmsg)) {
        return RBIPC_ERR_SYS;
    }
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(count * sizeof(int));
    memcpy(CMSG_DATA(cmsg), fds, count * sizeof(int));

    ssize_t ret = sendmsg(socket_fd, &msg, 0);
    if (count > 8 && cmsg_buf != stack_cmsg) {
        free(cmsg_buf);
    }
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

    /*
     * recvmsg() with SCM_RIGHTS *installs* the received fds in this process as
     * part of the syscall, regardless of what we do next. We therefore must
     * account for every fd deposited by the kernel: fds we do not return to the
     * caller must be closed, and if no usable SCM_RIGHTS block exists, every
     * installed fd must be closed to avoid an fd-exhaustion leak.
     *
     * The control buffer may contain several cmsghdrs (e.g. SCM_CREDENTIALS
     * followed by one or more SCM_RIGHTS blocks). We walk all of them with
     * CMSG_NXTHDR, and only consume the first SCM_RIGHTS block, closing any
     * deskriptors in later blocks and any excess beyond max_count.
     */
    size_t copy_count = 0;
    bool found_rights = false;
    const size_t control_len = (size_t)msg.msg_controllen;

    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL;
        cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        /* Validate the header against the actual received control length so a
         * malformed/oversized cmsg_len cannot drive reads past the buffer that
         * the kernel set up. */
        const unsigned char *chdr = (const unsigned char *)cmsg;
        if ((size_t)(chdr - (const unsigned char *)msg.msg_control) + sizeof(struct cmsghdr) > control_len) {
            break;
        }
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
            continue; /* Credentials/other blocks carry no fds; skip. */
        }

        size_t hdr_sz = (cmsg->cmsg_len > sizeof(struct cmsghdr))
                            ? (size_t)(cmsg->cmsg_len - sizeof(struct cmsghdr))
                            : 0;
        /* cmsg_len includes header + padding; bound the payload by the space
         * actually present and by CMSG_SPACE for one int per fd. */
        size_t span = (size_t)cmsg->cmsg_len;
        if (span > control_len - (size_t)(chdr - (const unsigned char *)msg.msg_control)) {
            span = control_len - (size_t)(chdr - (const unsigned char *)msg.msg_control);
        }
        size_t fds_present = (span >= sizeof(struct cmsghdr)) ? (span - sizeof(struct cmsghdr)) / sizeof(int) : 0;
        size_t num_fds = (hdr_sz / sizeof(int));
        if (num_fds > fds_present) num_fds = fds_present;

        if (num_fds == 0) {
            continue;
        }

        int *fds = (int *)CMSG_DATA(cmsg);
        if (!found_rights) {
            /* First (and only) SCM_RIGHTS block we honor. */
            found_rights = true;
            size_t keep = (num_fds < max_count) ? num_fds : max_count;
            for (size_t i = 0; i < keep; ++i) {
                out_fds[i] = fds[i];
            }
            copy_count = keep;
            /* Close any fds beyond the caller's requested count. */
            for (size_t i = keep; i < num_fds; ++i) {
                if (rbipc_is_valid_fd(fds[i])) {
                    close(fds[i]);
                }
            }
        } else {
            /* Additional SCM_RIGHTS blocks are surplus; close all of them. */
            for (size_t i = 0; i < num_fds; ++i) {
                if (rbipc_is_valid_fd(fds[i])) {
                    close(fds[i]);
                }
            }
        }
    }

    if (!found_rights) {
        /* No usable SCM_RIGHTS block encountered. Any fds the kernel installed
         * are unreachable here because we skipped non-rights blocks; the drain
         * above accounts for rights blocks. If none existed, nothing to close. */
        *out_count = 0;
        return RBIPC_ERR_SYS;
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
