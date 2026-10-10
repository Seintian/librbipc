/**
 * @file test_e2e_crash.c
 * @brief Dead-peer crash recovery test
 */

#include "rbipc.h"
#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>

#define TEST_CRASH_NAME "/rbipc_crash_test"

static void test_batch_crash_recovery(void) {
    const char *shm_name = "/rbipc_batch_crash_test";
    rbipc_destroy(shm_name);

    rbipc_ring_t *ring = NULL;
    int rc = rbipc_create(shm_name, 32, 64, &ring);
    assert(rc == RBIPC_OK);

    pid_t pid = fork();
    if (pid == 0) {
        rbipc_ring_t *child_ring = NULL;
        int c_rc = rbipc_attach(shm_name, &child_ring);
        assert(c_rc == RBIPC_OK);

        rbipc_iovec_t iovecs[4];
        uint32_t reserved = 0;
        c_rc = rbipc_reserve_write_batch(child_ring, 4, iovecs, &reserved);
        assert(c_rc == RBIPC_OK);
        assert(reserved == 4);

        /* Abruptly terminate holding 4 reserved slots */
        kill(getpid(), SIGKILL);
        _exit(1);
    }

    int status;
    waitpid(pid, &status, 0);

    /* Consumer calls batch acquire across the 4 reserved slots */
    rbipc_rovec_t rovecs[4];
    uint32_t acquired = 0;
    rc = rbipc_read_acquire_batch(ring, 4, rovecs, &acquired);
    assert(rc == RBIPC_OK);
    assert(acquired == 4);

    uint32_t release_tickets[4];
    for (uint32_t i = 0; i < 4; ++i) {
        /* Poisoned slots must have buf == NULL and len == 0 */
        assert(rovecs[i].buf == NULL);
        assert(rovecs[i].len == 0);
        assert(rovecs[i].ticket == i);
        release_tickets[i] = rovecs[i].ticket;
    }

    /* Release batch of recovered poisoned slots */
    rc = rbipc_read_release_batch(ring, 4, release_tickets);
    assert(rc == RBIPC_OK);

    /* Verify subsequent batch operations proceed normally */
    rbipc_iovec_t w_iovecs[2];
    uint32_t w_res = 0;
    rc = rbipc_reserve_write_batch(ring, 2, w_iovecs, &w_res);
    assert(rc == RBIPC_OK);
    assert(w_res == 2);
    strcpy((char *)w_iovecs[0].buf, "BATCH_MSG_0");
    strcpy((char *)w_iovecs[1].buf, "BATCH_MSG_1");
    uint32_t w_tickets[2] = { w_iovecs[0].ticket, w_iovecs[1].ticket };
    uint32_t w_lens[2] = { 12, 12 };
    rc = rbipc_commit_write_batch(ring, 2, w_tickets, w_lens);
    assert(rc == RBIPC_OK);

    rbipc_rovec_t r_rovecs[2];
    uint32_t r_acq = 0;
    rc = rbipc_read_acquire_batch(ring, 2, r_rovecs, &r_acq);
    assert(rc == RBIPC_OK);
    assert(r_acq == 2);
    assert(r_rovecs[0].buf != NULL && strcmp((const char *)r_rovecs[0].buf, "BATCH_MSG_0") == 0);
    assert(r_rovecs[1].buf != NULL && strcmp((const char *)r_rovecs[1].buf, "BATCH_MSG_1") == 0);
    uint32_t r_tickets[2] = { r_rovecs[0].ticket, r_rovecs[1].ticket };
    rc = rbipc_read_release_batch(ring, 2, r_tickets);
    assert(rc == RBIPC_OK);

    rbipc_detach(ring);
    rbipc_destroy(shm_name);
}

int main(void) {
    printf("[test_e2e_crash] Running dead-peer crash recovery test...\n");
    rbipc_destroy(TEST_CRASH_NAME);

    rbipc_ring_t *ring = NULL;
    int rc = rbipc_create(TEST_CRASH_NAME, 16, 64, &ring);
    assert(rc == RBIPC_OK);

    pid_t pid = fork();
    if (pid == 0) {
        /* Child process */
        rbipc_ring_t *child_ring = NULL;
        int c_rc = rbipc_attach(TEST_CRASH_NAME, &child_ring);
        assert(c_rc == RBIPC_OK);

        void *buf = NULL;
        uint32_t ticket = 0;
        c_rc = rbipc_reserve_write(child_ring, 32, &buf, &ticket);
        assert(c_rc == RBIPC_OK);
        assert(ticket == 0);

        /* Abrupt ungraceful termination */
        kill(getpid(), SIGKILL);
        _exit(1);
    }

    /* Parent waits for child to terminate */
    int status;
    waitpid(pid, &status, 0);

    /* Consumer attempts to read slot held by deceased child */
    const void *r_buf = NULL;
    uint32_t r_len = 0;
    uint32_t r_ticket = 0;

    rc = rbipc_read_acquire(ring, &r_buf, &r_len, &r_ticket);
    assert(rc == RBIPC_ERR_POISONED);
    assert(r_ticket == 0);

    /* Release poisoned slot */
    rc = rbipc_read_release(ring, r_ticket);
    assert(rc == RBIPC_OK);

    /* Verify subsequent normal communication works */
    void *w_buf = NULL;
    uint32_t w_ticket = 0;
    rc = rbipc_reserve_write(ring, 32, &w_buf, &w_ticket);
    assert(rc == RBIPC_OK);
    assert(w_ticket == 1);
    strcpy((char *)w_buf, "RECOVERY_OK");
    rc = rbipc_commit_write(ring, w_ticket, 32);
    assert(rc == RBIPC_OK);

    rc = rbipc_read_acquire(ring, &r_buf, &r_len, &r_ticket);
    assert(rc == RBIPC_OK);
    assert(r_ticket == 1);
    assert(strcmp((const char *)r_buf, "RECOVERY_OK") == 0);
    rc = rbipc_read_release(ring, r_ticket);
    assert(rc == RBIPC_OK);

    rbipc_detach(ring);
    rbipc_destroy(TEST_CRASH_NAME);

    test_batch_crash_recovery();

    printf("[test_e2e_crash] Dead-peer crash recovery test passed!\n");
    return 0;
}
