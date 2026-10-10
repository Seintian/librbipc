/**
 * @file test_e2e_crash.c
 * @brief Dead-peer crash recovery test
 */

#include "rbipc.h"
#include "../src/rbipc_internal.h"
#include "../src/rbipc_slot.h"
#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <stdatomic.h>

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

    uint64_t release_tickets[4];
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
    uint64_t w_tickets[2] = { w_iovecs[0].ticket, w_iovecs[1].ticket };
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
    uint64_t r_tickets[2] = { r_rovecs[0].ticket, r_rovecs[1].ticket };
    rc = rbipc_read_release_batch(ring, 2, r_tickets);
    assert(rc == RBIPC_OK);

    rbipc_detach(ring);
    rbipc_destroy(shm_name);
}

static void test_consumer_crash_recovery(void) {
    const char *shm_name = "/rbipc_consumer_crash_test";
    rbipc_destroy(shm_name);

    rbipc_ring_t *ring = NULL;
    int rc = rbipc_create(shm_name, 4, 64, &ring);
    assert(rc == RBIPC_OK);

    /* Producer writes 4 messages filling the ring */
    for (uint32_t i = 0; i < 4; ++i) {
        void *buf = NULL;
        uint64_t ticket = 0;
        rc = rbipc_reserve_write(ring, 16, &buf, &ticket);
        assert(rc == RBIPC_OK);
        assert(ticket == i);
        snprintf((char *)buf, 16, "MSG_%u", i);
        rc = rbipc_commit_write(ring, ticket, 16);
        assert(rc == RBIPC_OK);
    }

    pid_t pid = fork();
    if (pid == 0) {
        rbipc_ring_t *child_ring = NULL;
        int c_rc = rbipc_attach(shm_name, &child_ring);
        assert(c_rc == RBIPC_OK);

        const void *buf = NULL;
        uint32_t len = 0;
        uint64_t ticket = 0;
        /* Child consumer acquires slot 0 (ticket 0) */
        c_rc = rbipc_read_acquire(child_ring, &buf, &len, &ticket);
        assert(c_rc == RBIPC_OK);
        assert(ticket == 0);

        /* Abruptly terminate holding acquired slot 0 without releasing */
        kill(getpid(), SIGKILL);
        _exit(1);
    }

    int status;
    waitpid(pid, &status, 0);

    /* Producer attempts to produce message 5 (ticket 4), which wraps to slot 0 */
    void *w_buf = NULL;
    uint64_t w_ticket = 0;
    rc = rbipc_reserve_write(ring, 16, &w_buf, &w_ticket);
    assert(rc == RBIPC_OK);
    assert(w_ticket == 4);
    strcpy((char *)w_buf, "RECOVERED_MSG");
    rc = rbipc_commit_write(ring, w_ticket, 16);
    assert(rc == RBIPC_OK);

    rbipc_detach(ring);
    rbipc_destroy(shm_name);
}

static void test_ticket_hole_crash_recovery(void) {
    const char *shm_name = "/rbipc_ticket_hole_test";
    rbipc_destroy(shm_name);

    rbipc_ring_t *ring = NULL;
    int rc = rbipc_create(shm_name, 16, 64, &ring);
    assert(rc == RBIPC_OK);

    pid_t pid = fork();
    if (pid == 0) {
        rbipc_ring_t *child_ring = NULL;
        int c_rc = rbipc_attach(shm_name, &child_ring);
        assert(c_rc == RBIPC_OK);

        /* Simulates dead producer crash: marks RESERVING, claims ticket CAS, and dies */
        rbipc_slot_t *slots = (rbipc_slot_t *)child_ring->slots;
        rbipc_slot_mark_reserving(&slots[0], getpid());
        uint64_t t = 0;
        bool claimed = atomic_compare_exchange_strong(&child_ring->hdr->write_ticket, &t, 1);
        assert(claimed == true);

        kill(getpid(), SIGKILL);
        _exit(1);
    }

    int status;
    waitpid(pid, &status, 0);

    /* Consumer attempts to read ticket 0, which is an abandoned ticket hole */
    const void *r_buf = NULL;
    uint32_t r_len = 0;
    uint64_t r_ticket = 0;

    rc = rbipc_read_acquire(ring, &r_buf, &r_len, &r_ticket);
    assert(rc == RBIPC_ERR_POISONED);
    assert(r_ticket == 0);

    /* Release recovered poisoned ticket hole */
    rc = rbipc_read_release(ring, r_ticket);
    assert(rc == RBIPC_OK);

    /* Normal producer writes ticket 1 */
    void *w_buf = NULL;
    uint64_t w_ticket = 0;
    rc = rbipc_reserve_write(ring, 32, &w_buf, &w_ticket);
    assert(rc == RBIPC_OK);
    assert(w_ticket == 1);
    strcpy((char *)w_buf, "AFTER_HOLE_OK");
    rc = rbipc_commit_write(ring, w_ticket, 32);
    assert(rc == RBIPC_OK);

    rc = rbipc_read_acquire(ring, &r_buf, &r_len, &r_ticket);
    assert(rc == RBIPC_OK);
    assert(r_ticket == 1);
    assert(strcmp((const char *)r_buf, "AFTER_HOLE_OK") == 0);
    rc = rbipc_read_release(ring, r_ticket);
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
        uint64_t ticket = 0;
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
    uint64_t r_ticket = 0;

    rc = rbipc_read_acquire(ring, &r_buf, &r_len, &r_ticket);
    assert(rc == RBIPC_ERR_POISONED);
    assert(r_ticket == 0);

    /* Release poisoned slot */
    rc = rbipc_read_release(ring, r_ticket);
    assert(rc == RBIPC_OK);

    /* Verify subsequent normal communication works */
    void *w_buf = NULL;
    uint64_t w_ticket = 0;
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
    test_consumer_crash_recovery();
    test_ticket_hole_crash_recovery();

    printf("[test_e2e_crash] Dead-peer crash recovery test passed!\n");
    return 0;
}
