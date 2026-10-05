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

    printf("[test_e2e_crash] Dead-peer crash recovery test passed!\n");
    return 0;
}
