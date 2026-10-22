/**
 * @file comp_throughput.c
 * @brief 1P-1C streaming throughput benchmarks for competitors.
 */

#include "comp_common.h"

result_t bench_rbipc_tput(size_t msgsz, uint64_t iters, transfer_semantic_t mode, uint32_t flags, const char *label) {
    result_t r = {};
    snprintf(r.name, sizeof(r.name), "%s", label ? label : "librbipc");
    r.msgsz = msgsz; r.iters = iters;

    const char *shm = "/rbench_rbipc_tput";
    rbipc_ring_t *rp = NULL, *rc = NULL;
    uint32_t slot = (mode == TRANSFER_VALUE_COPY) ? (uint32_t)(msgsz + 64) : 64;
    uint32_t req_sz = (mode == TRANSFER_VALUE_COPY) ? (uint32_t)msgsz : (uint32_t)sizeof(uintptr_t);
    rbipc_destroy(shm);
    if (rbipc_create_ex(shm, RING_CAPACITY, slot, RBIPC_SHM_NAMED | flags, 0660, &rp) != RBIPC_OK ||
        rbipc_attach(shm, &rc) != RBIPC_OK) return r;
    uint32_t wait_pol = (flags & RBIPC_FLAG_TRUSTED) ? RBIPC_WAIT_ADAPTIVE : RBIPC_WAIT_ZERO_SPIN;
    rbipc_set_wait_policy(rp, wait_pol);
    rbipc_set_wait_policy(rc, wait_pol);

    int sync_pipe[2];
    assert(pipe(sync_pipe) == 0);
    char *pay = aligned_alloc(64, msgsz > 64 ? msgsz : 64);
    memset(pay, 0xAB, msgsz);
    uint64_t t0 = 0, t1 = 0;

    pid_t pid = fork();
    if (pid == 0) {
        close(sync_pipe[0]);
        for (uint64_t n = 0; n < iters; n++) {
            const void *buf; uint64_t tk; uint32_t ln;
            if (RBIPC_UNLIKELY(rbipc_read_acquire(rc, &buf, &ln, &tk) != RBIPC_OK)) {
                break;
            }
            if (mode == TRANSFER_REFERENCE_PASSING) {
                volatile uintptr_t ptr = *(const uintptr_t *)buf;
                (void)ptr;
            }
            rbipc_read_release(rc, tk);
        }
        char sig[4] = "OK";
        ssize_t w = write(sync_pipe[1], sig, sizeof(sig));
        (void)w;
        close(sync_pipe[1]);
        rbipc_detach(rc);
        _exit(0);
    } else {
        close(sync_pipe[1]);
        t0 = comp_get_ns();
        for (uint64_t i = 0; i < iters; i++) {
            void *buf; uint64_t tk;
            if (RBIPC_UNLIKELY(rbipc_reserve_write(rp, req_sz, &buf, &tk) != RBIPC_OK)) {
                break;
            }
            if (mode == TRANSFER_VALUE_COPY) {
                memcpy(buf, pay, msgsz);
            } else {
                *(uintptr_t *)buf = (uintptr_t)(pay + (i % RING_CAPACITY));
            }
            rbipc_commit_write(rp, tk, req_sz);
        }
        char sig[4];
        ssize_t s = read(sync_pipe[0], sig, sizeof(sig));
        (void)s;
        t1 = comp_get_ns();
        close(sync_pipe[0]);
        waitpid(pid, NULL, 0);
    }

    rbipc_detach(rp);
    rbipc_destroy(shm);
    free(pay);

    r.elapsed_ns = t1 - t0;
    r.tput_ops   = (double)iters / ((double)r.elapsed_ns / 1e9);
    r.tput_mbs   = r.tput_ops * (double)msgsz / (1024.0 * 1024.0);
    return r;
}

result_t bench_rte_tput(size_t msgsz, uint64_t iters, transfer_semantic_t mode) {
    result_t r = {};
    snprintf(r.name, sizeof(r.name), "rte_ring (DPDK alg)");
    r.msgsz = msgsz; r.iters = iters;

    size_t pool_sz = RING_CAPACITY * (msgsz > 64 ? msgsz : 64);
    char *pool = mmap(NULL, pool_sz, PROT_READ|PROT_WRITE,
                      MAP_SHARED|MAP_ANONYMOUS, -1, 0);
    memset(pool, 0xBC, pool_sz);
    char *pay = aligned_alloc(64, msgsz > 64 ? msgsz : 64);
    memset(pay, 0xBC, msgsz);
    rte_ring_t *ring = rte_ring_alloc(RING_CAPACITY);
    int sync_pipe[2];
    assert(pipe(sync_pipe) == 0);
    uint64_t t0 = 0, t1 = 0;

    pid_t pid = fork();
    if (pid == 0) {
        close(sync_pipe[0]);
        uintptr_t v;
        for (uint64_t n = 0; n < iters; n++) {
            rte_dequeue_wait(ring, &v);
        }
        char sig[4] = "OK";
        ssize_t w = write(sync_pipe[1], sig, sizeof(sig));
        (void)w;
        close(sync_pipe[1]);
        _exit(0);
    } else {
        close(sync_pipe[1]);
        t0 = comp_get_ns();
        for (uint64_t i = 0; i < iters; i++) {
            char *slot = pool + (i % RING_CAPACITY) * msgsz;
            if (mode == TRANSFER_VALUE_COPY) {
                memcpy(slot, pay, msgsz);
            }
            rte_enqueue_wait(ring, (uintptr_t)slot);
        }
        char sig[4];
        ssize_t s = read(sync_pipe[0], sig, sizeof(sig));
        (void)s;
        t1 = comp_get_ns();
        close(sync_pipe[0]);
        waitpid(pid, NULL, 0);
    }

    rte_ring_free(ring);
    munmap(pool, pool_sz);
    free(pay);
    r.elapsed_ns = t1 - t0;
    r.tput_ops   = (double)iters / ((double)r.elapsed_ns / 1e9);
    r.tput_mbs   = r.tput_ops * (double)msgsz / (1024.0 * 1024.0);
    return r;
}

result_t bench_disruptor_tput(size_t msgsz, uint64_t iters, transfer_semantic_t mode) {
    result_t r = {};
    snprintf(r.name, sizeof(r.name), "LMAX Disruptor");
    r.msgsz = msgsz; r.iters = iters;

    size_t pool_sz = RING_CAPACITY * (msgsz > 64 ? msgsz : 64);
    char *pool = mmap(NULL, pool_sz, PROT_READ|PROT_WRITE,
                      MAP_SHARED|MAP_ANONYMOUS, -1, 0);
    memset(pool, 0xAA, pool_sz);
    char *pay = aligned_alloc(64, msgsz > 64 ? msgsz : 64);
    memset(pay, 0xAA, msgsz);
    disruptor_ring_t *ring = disruptor_alloc(RING_CAPACITY);
    int sync_pipe[2];
    assert(pipe(sync_pipe) == 0);
    uint64_t t0 = 0, t1 = 0;

    pid_t pid = fork();
    if (pid == 0) {
        close(sync_pipe[0]);
        uintptr_t v;
        for (uint64_t n = 0; n < iters; n++) {
            while (!disruptor_consume_wp(ring, (int64_t)iters, &v)) {
                __asm__ volatile("pause" ::: "memory");
            }
        }
        char sig[4] = "OK";
        ssize_t w = write(sync_pipe[1], sig, sizeof(sig));
        (void)w;
        close(sync_pipe[1]);
        _exit(0);
    } else {
        close(sync_pipe[1]);
        t0 = comp_get_ns();
        for (uint64_t i = 0; i < iters; i++) {
            char *slot = pool + (i % RING_CAPACITY) * msgsz;
            if (mode == TRANSFER_VALUE_COPY) {
                memcpy(slot, pay, msgsz);
            }
            disruptor_publish_sp(ring, (int64_t)i, (uintptr_t)slot);
        }
        char sig[4];
        ssize_t s = read(sync_pipe[0], sig, sizeof(sig));
        (void)s;
        t1 = comp_get_ns();
        close(sync_pipe[0]);
        waitpid(pid, NULL, 0);
    }

    disruptor_free(ring);
    munmap(pool, pool_sz);
    free(pay);
    r.elapsed_ns = t1 - t0;
    r.tput_ops   = (double)iters / ((double)r.elapsed_ns / 1e9);
    r.tput_mbs   = r.tput_ops * (double)msgsz / (1024.0 * 1024.0);
    return r;
}

result_t bench_mq_tput(size_t msgsz, uint64_t iters) {
    result_t r = {};
    snprintf(r.name, sizeof(r.name), "POSIX mqueue");
    if (msgsz > 8192) msgsz = 8192;
    r.msgsz = msgsz; r.iters = iters;

    mqd_t mq = mq_make(MQ_NAME_AB, msgsz, 10);
    if (mq == (mqd_t)-1) return r;

    int sync_pipe[2];
    assert(pipe(sync_pipe) == 0);
    char *pay = aligned_alloc(64, msgsz > 64 ? msgsz : 64);
    memset(pay, 0xCD, msgsz);
    char *rbuf = aligned_alloc(64, msgsz > 64 ? msgsz : 64);
    uint64_t t0 = 0, t1 = 0;

    pid_t pid = fork();
    if (pid == 0) {
        close(sync_pipe[0]);
        unsigned int prio;
        for (uint64_t n = 0; n < iters; n++) {
            ssize_t s = mq_receive(mq, rbuf, msgsz, &prio);
            assert(s > 0);
        }
        char sig[4] = "OK";
        ssize_t w = write(sync_pipe[1], sig, sizeof(sig));
        (void)w;
        close(sync_pipe[1]);
        mq_close(mq);
        _exit(0);
    } else {
        close(sync_pipe[1]);
        t0 = comp_get_ns();
        for (uint64_t i = 0; i < iters; i++) {
            while (mq_send(mq, pay, msgsz, MQ_PRIO) < 0) {
                if (errno != EAGAIN && errno != EINTR) break;
            }
        }
        char sig[4];
        ssize_t s = read(sync_pipe[0], sig, sizeof(sig));
        (void)s;
        t1 = comp_get_ns();
        close(sync_pipe[0]);
        waitpid(pid, NULL, 0);
    }

    mq_close(mq); mq_unlink(MQ_NAME_AB);
    free(pay); free(rbuf);
    r.elapsed_ns = t1 - t0;
    r.tput_ops   = (double)iters / ((double)r.elapsed_ns / 1e9);
    r.tput_mbs   = r.tput_ops * (double)msgsz / (1024.0 * 1024.0);
    return r;
}

result_t bench_uds_tput(size_t msgsz, uint64_t iters) {
    result_t r = {};
    snprintf(r.name, sizeof(r.name), "AF_UNIX DGRAM");
    if (msgsz > 65536) msgsz = 65536;
    r.msgsz = msgsz; r.iters = iters;

    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0);
    int buf = 2 << 20;
    setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
    setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));

    int sync_pipe[2];
    assert(pipe(sync_pipe) == 0);
    char *pay = aligned_alloc(64, msgsz > 64 ? msgsz : 64);
    memset(pay, 0xDE, msgsz);
    char *rbuf = aligned_alloc(64, msgsz > 64 ? msgsz : 64);
    uint64_t t0 = 0, t1 = 0;

    pid_t pid = fork();
    if (pid == 0) {
        close(sync_pipe[0]);
        close(sv[0]);
        for (uint64_t n = 0; n < iters; n++) {
            ssize_t s = recv(sv[1], rbuf, msgsz, 0);
            assert(s > 0);
        }
        close(sv[1]);
        char sig[4] = "OK";
        ssize_t w = write(sync_pipe[1], sig, sizeof(sig));
        (void)w;
        close(sync_pipe[1]);
        _exit(0);
    } else {
        close(sync_pipe[1]);
        close(sv[1]);
        t0 = comp_get_ns();
        for (uint64_t i = 0; i < iters; i++) {
            ssize_t s = send(sv[0], pay, msgsz, 0);
            assert(s > 0);
        }
        char sig[4];
        ssize_t s = read(sync_pipe[0], sig, sizeof(sig));
        (void)s;
        t1 = comp_get_ns();
        close(sv[0]);
        close(sync_pipe[0]);
        waitpid(pid, NULL, 0);
    }

    free(pay); free(rbuf);
    r.elapsed_ns = t1 - t0;
    r.tput_ops   = (double)iters / ((double)r.elapsed_ns / 1e9);
    r.tput_mbs   = r.tput_ops * (double)msgsz / (1024.0 * 1024.0);
    return r;
}
