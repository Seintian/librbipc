/**
 * @file deep_reactor.c
 * @brief Eventfd reactor asynchronous epoll notification latency micro-benchmark
 */

#include "deep_common.h"

void run_reactor_bench(int iterations, reactor_bench_result_t *out) {
    rbipc_destroy(DEEP_SHM_A);
    rbipc_ring_t *ring = NULL;
    assert(rbipc_create(DEEP_SHM_A, 256, 64, &ring) == RBIPC_OK);

    int efd = -1;
    assert(rbipc_enable_eventfd(ring, &efd) == RBIPC_OK);

    int epfd = epoll_create1(0);
    assert(epfd >= 0);

    struct epoll_event ev = { .events = EPOLLIN, .data.fd = efd };
    assert(epoll_ctl(epfd, EPOLL_CTL_ADD, efd, &ev) == 0);

    uint64_t t0 = get_time_ns();
    for (int i = 0; i < iterations; ++i) {
        void *wbuf = NULL;
        uint64_t wticket = 0;
        assert(rbipc_reserve_write(ring, 32, &wbuf, &wticket) == RBIPC_OK);
        assert(rbipc_commit_write(ring, wticket, 32) == RBIPC_OK);

        struct epoll_event events[1];
        int nfds = epoll_wait(epfd, events, 1, 1000);
        assert(nfds == 1);

        uint64_t val = 0;
        assert(read(efd, &val, sizeof(val)) == (ssize_t)sizeof(val));

        const void *rbuf = NULL;
        uint32_t rlen = 0;
        uint64_t rticket = 0;
        assert(rbipc_read_acquire(ring, &rbuf, &rlen, &rticket) == RBIPC_OK);
        rbipc_read_release(ring, rticket);
    }
    uint64_t t1 = get_time_ns();

    close(epfd);
    rbipc_detach(ring);
    rbipc_destroy(DEEP_SHM_A);

    double sec = (double)(t1 - t0) / (double)RBIPC_NSEC_PER_SEC;
    out->events_per_sec = (double)iterations / sec;
    out->avg_latency_ns = (sec * (double)RBIPC_NSEC_PER_SEC) / (double)iterations;
}
