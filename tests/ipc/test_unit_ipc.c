/**
 * @file test_unit_ipc.c
 * @brief Unit tests for UNIX domain socket SCM_RIGHTS descriptor passing utilities.
 */

#include "rbipc.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/socket.h>

static void test_ipc_invalid_params(void) {
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    rbipc_ring_t *ring = NULL;
    assert(rbipc_send_fd(-1, NULL) == RBIPC_ERR_INVAL);
    assert(rbipc_send_fd(sv[0], NULL) == RBIPC_ERR_INVAL);
    assert(rbipc_recv_fd(-1, &ring) == RBIPC_ERR_INVAL);
    assert(rbipc_recv_fd(sv[1], NULL) == RBIPC_ERR_INVAL);
    assert(rbipc_recv_fd_ex(-1, RBIPC_ROLE_CONSUMER, &ring) == RBIPC_ERR_INVAL);
    assert(rbipc_recv_fd_ex(sv[1], 0, &ring) == RBIPC_ERR_INVAL);
    assert(rbipc_recv_fd_ex(sv[1], 0xFFFF, &ring) == RBIPC_ERR_INVAL);

    int out_fd = -1;
    assert(rbipc_ipc_send_fd(-1, sv[0]) == RBIPC_ERR_INVAL);
    assert(rbipc_ipc_send_fd(sv[0], -1) == RBIPC_ERR_INVAL);
    assert(rbipc_ipc_recv_fd(-1, &out_fd) == RBIPC_ERR_INVAL);
    assert(rbipc_ipc_recv_fd(sv[1], NULL) == RBIPC_ERR_INVAL);

    close(sv[0]);
    close(sv[1]);
}

static void test_ipc_send_recv_ring(void) {
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    /* 1. Create anonymous ring buffer (memfd, no path in /dev/shm) */
    rbipc_ring_t *sender_ring = NULL;
    int rc = rbipc_create(NULL, 64, 256, &sender_ring);
    assert(rc == RBIPC_OK);
    assert(sender_ring != NULL);

    /* 2. Write a message into the ring before sending */
    void *wbuf = NULL;
    uint64_t wticket = 0;
    const char *test_msg = "Zero-trust memfd passing via SCM_RIGHTS";
    uint32_t msg_len = (uint32_t)strlen(test_msg) + 1;

    rc = rbipc_reserve_write(sender_ring, msg_len, &wbuf, &wticket);
    assert(rc == RBIPC_OK);
    memcpy(wbuf, test_msg, msg_len);
    rc = rbipc_commit_write(sender_ring, wticket, msg_len);
    assert(rc == RBIPC_OK);

    /* 3. Send ring descriptor across UNIX socket */
    rc = rbipc_send_fd(sv[0], sender_ring);
    assert(rc == RBIPC_OK);

    /* 4. Receive ring descriptor on other end and attach as consumer */
    rbipc_ring_t *recv_ring = NULL;
    rc = rbipc_recv_fd_ex(sv[1], RBIPC_ROLE_CONSUMER, &recv_ring);
    assert(rc == RBIPC_OK);
    assert(recv_ring != NULL);

    /* 5. Read message through received ring handle */
    const void *rbuf = NULL;
    uint32_t rlen = 0;
    uint64_t rticket = 0;
    rc = rbipc_read_acquire(recv_ring, &rbuf, &rlen, &rticket);
    assert(rc == RBIPC_OK);
    assert(rlen == msg_len);
    assert(strcmp((const char *)rbuf, test_msg) == 0);
    rc = rbipc_read_release(recv_ring, rticket);
    assert(rc == RBIPC_OK);

    /* 6. Clean up */
    rbipc_detach(recv_ring);
    rbipc_detach(sender_ring);
    close(sv[0]);
    close(sv[1]);
}

static void test_ipc_peer_credentials_and_auth(void) {
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    rbipc_peer_cred_t cred;
    memset(&cred, 0, sizeof(cred));
    assert(rbipc_ipc_get_peer_cred(-1, &cred) == RBIPC_ERR_INVAL);
    assert(rbipc_ipc_get_peer_cred(sv[0], NULL) == RBIPC_ERR_INVAL);

    int rc = rbipc_ipc_get_peer_cred(sv[0], &cred);
    assert(rc == RBIPC_OK);
    assert(cred.pid == getpid());
    assert(cred.uid == getuid());
    assert(cred.gid == getgid());

    assert(rbipc_ipc_verify_peer(-1, cred.uid, cred.gid) == RBIPC_ERR_INVAL);
    assert(rbipc_ipc_verify_peer(sv[0], cred.uid, cred.gid) == RBIPC_OK);
    assert(rbipc_ipc_verify_peer(sv[0], cred.uid, (gid_t)-1) == RBIPC_OK);
    assert(rbipc_ipc_verify_peer(sv[0], (uid_t)-1, cred.gid) == RBIPC_OK);
    assert(rbipc_ipc_verify_peer(sv[0], cred.uid + 9999, cred.gid) == RBIPC_ERR_INVAL);
    assert(rbipc_ipc_verify_peer(sv[0], cred.uid, cred.gid + 9999) == RBIPC_ERR_INVAL);

    rbipc_ring_t *ring = NULL;
    assert(rbipc_create(NULL, 64, 256, &ring) == RBIPC_OK);

    /* Failed send due to wrong expected peer UID */
    assert(rbipc_send_fd_auth(sv[0], ring, cred.uid + 9999, cred.gid) == RBIPC_ERR_INVAL);

    /* Successful authenticated send and receive */
    assert(rbipc_send_fd_auth(sv[0], ring, cred.uid, cred.gid) == RBIPC_OK);

    rbipc_ring_t *recv_ring = NULL;
    assert(rbipc_recv_fd_auth(sv[1], RBIPC_ROLE_CONSUMER, cred.uid, cred.gid, &recv_ring) == RBIPC_OK);
    assert(recv_ring != NULL);

    rbipc_detach(recv_ring);
    rbipc_detach(ring);
    close(sv[0]);
    close(sv[1]);
}

int main(void) {
    printf("[test_unit_ipc] Running UNIX domain socket SCM_RIGHTS tests...\n");
    test_ipc_invalid_params();
    test_ipc_send_recv_ring();
    test_ipc_peer_credentials_and_auth();
    printf("[test_unit_ipc] All tests passed!\n");
    return 0;
}
