/**
 * @file test_unit_reactor.c
 * @brief Unit tests for asynchronous eventfd and epoll reactor integration (Point 3.2).
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "rbipc.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/epoll.h>

#define REACTOR_RING_NAME "/test_reactor_ring"
#define REACTOR_CAPACITY 64
#define REACTOR_SLOT_SIZE 128

static void test_reactor_eventfd_lifecycle(void) {
    rbipc_destroy(REACTOR_RING_NAME);

    rbipc_ring_t *ring = NULL;
    int rc = rbipc_create(REACTOR_RING_NAME, REACTOR_CAPACITY, REACTOR_SLOT_SIZE, &ring);
    assert(rc == RBIPC_OK);

    int out_fd = -1;
    assert(rbipc_get_eventfd(ring, &out_fd) == RBIPC_ERR_NOTSUP);

    rc = rbipc_enable_eventfd(ring, &out_fd);
    assert(rc == RBIPC_OK);
    assert(out_fd >= 0);

    int out_fd2 = -1;
    rc = rbipc_get_eventfd(ring, &out_fd2);
    assert(rc == RBIPC_OK);
    assert(out_fd2 == out_fd);

    /* Idempotent enable */
    int out_fd3 = -1;
    rc = rbipc_enable_eventfd(ring, &out_fd3);
    assert(rc == RBIPC_OK);
    assert(out_fd3 == out_fd);

    /* Drain on empty should succeed */
    rc = rbipc_eventfd_drain(ring);
    assert(rc == RBIPC_OK);

    /* Verify eventfd_waiters is tracked in shared memory via public telemetry */
    rbipc_ring_t *peer = NULL;
    rc = rbipc_attach(REACTOR_RING_NAME, &peer);
    assert(rc == RBIPC_OK);

    rbipc_stats_t stats_before;
    assert(rbipc_get_stats(peer, &stats_before) == RBIPC_OK);
    assert(stats_before.eventfd_waiters == 1);

    /* Detaching the ring with the enabled eventfd should clean up fd and decrement waiters */
    rbipc_detach(ring);

    rbipc_stats_t stats_after;
    assert(rbipc_get_stats(peer, &stats_after) == RBIPC_OK);
    assert(stats_after.eventfd_waiters == 0);

    rbipc_detach(peer);
    rbipc_destroy(REACTOR_RING_NAME);
}

static void test_reactor_epoll_integration(void) {
    rbipc_destroy(REACTOR_RING_NAME);

    rbipc_ring_t *ring = NULL;
    int rc = rbipc_create(REACTOR_RING_NAME, REACTOR_CAPACITY, REACTOR_SLOT_SIZE, &ring);
    assert(rc == RBIPC_OK);

    int event_fd = -1;
    rc = rbipc_enable_eventfd(ring, &event_fd);
    assert(rc == RBIPC_OK);
    assert(event_fd >= 0);

    int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    assert(epoll_fd >= 0);

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.fd = event_fd;
    assert(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, event_fd, &ev) == 0);

    struct epoll_event events[4];
    /* 1. epoll_wait should return 0 before any commits */
    int n = epoll_wait(epoll_fd, events, 4, 0);
    assert(n == 0);

    /* 2. Write and commit a message */
    void *buf = NULL;
    uint64_t ticket = 0;
    rc = rbipc_reserve_write(ring, 14, &buf, &ticket);
    assert(rc == RBIPC_OK);
    memcpy(buf, "Hello Reactor!", 14);
    rc = rbipc_commit_write(ring, ticket, 14);
    assert(rc == RBIPC_OK);

    /* 3. epoll_wait should return 1 event */
    n = epoll_wait(epoll_fd, events, 4, 50);
    assert(n == 1);
    assert(events[0].data.fd == event_fd);

    /* 4. Drain eventfd */
    assert(rbipc_eventfd_drain(ring) == RBIPC_OK);
    n = epoll_wait(epoll_fd, events, 4, 0);
    assert(n == 0);

    /* 5. Acquire and release */
    const void *rbuf = NULL;
    uint32_t rlen = 0;
    uint64_t rticket = 0;
    rc = rbipc_read_acquire(ring, &rbuf, &rlen, &rticket);
    assert(rc == RBIPC_OK);
    assert(rlen == 14);
    rc = rbipc_read_release(ring, rticket);
    assert(rc == RBIPC_OK);

    /* 6. Release slot should trigger eventfd */
    n = epoll_wait(epoll_fd, events, 4, 50);
    assert(n == 1);
    assert(rbipc_eventfd_drain(ring) == RBIPC_OK);

    /* 7. Batch commit triggers eventfd */
    rbipc_iovec_t iovecs[4];
    uint32_t reserved = 0;
    rc = rbipc_reserve_write_batch(ring, 4, iovecs, &reserved);
    assert(rc == RBIPC_OK && reserved == 4);
    uint64_t tickets[4] = {iovecs[0].ticket, iovecs[1].ticket, iovecs[2].ticket, iovecs[3].ticket};
    uint32_t lens[4] = {8, 8, 8, 8};
    rc = rbipc_commit_write_batch(ring, 4, tickets, lens);
    assert(rc == RBIPC_OK);

    n = epoll_wait(epoll_fd, events, 4, 50);
    assert(n == 1);
    assert(rbipc_eventfd_drain(ring) == RBIPC_OK);

    /* Batch release triggers eventfd */
    rbipc_rovec_t rovecs[4];
    uint32_t acquired = 0;
    rc = rbipc_read_acquire_batch(ring, 4, rovecs, &acquired);
    assert(rc == RBIPC_OK && acquired == 4);
    uint64_t rel_tickets[4] = {rovecs[0].ticket, rovecs[1].ticket, rovecs[2].ticket, rovecs[3].ticket};
    rc = rbipc_read_release_batch(ring, 4, rel_tickets);
    assert(rc == RBIPC_OK);

    n = epoll_wait(epoll_fd, events, 4, 50);
    assert(n == 1);
    assert(rbipc_eventfd_drain(ring) == RBIPC_OK);

    /* 8. Shutdown triggers eventfd */
    rc = rbipc_signal_shutdown(ring);
    assert(rc == RBIPC_OK);
    n = epoll_wait(epoll_fd, events, 4, 50);
    assert(n == 1);

    close(epoll_fd);
    rbipc_detach(ring);
    rbipc_destroy(REACTOR_RING_NAME);
}

int main(void) {
    printf("[test_unit_reactor] Running asynchronous eventfd reactor tests...\n");
    test_reactor_eventfd_lifecycle();
    test_reactor_epoll_integration();
    printf("[test_unit_reactor] All tests passed!\n");
    return 0;
}
