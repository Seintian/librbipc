/**
 * @file comp_latency.c
 * @brief Ping-Pong Half-RTT latency distribution benchmarks for competitors.
 */

#include "comp_common.h"

result_t bench_rbipc_lat(uint64_t iters) {
    result_t r = {};
    snprintf(r.name, sizeof(r.name), "librbipc");
    r.msgsz = 8; r.iters = iters;
    const char *sa = "/rbench_lat_a", *sb = "/rbench_lat_b";
    rbipc_ring_t *rp = NULL, *rc = NULL, *rp2 = NULL, *rc2 = NULL;
    rbipc_destroy(sa); rbipc_destroy(sb);
    assert(rbipc_create(sa, 256, 72, &rp) == RBIPC_OK);
    assert(rbipc_create(sb, 256, 72, &rp2) == RBIPC_OK);
    assert(rbipc_attach(sa, &rc) == RBIPC_OK);
    assert(rbipc_attach(sb, &rc2) == RBIPC_OK);
    rbipc_set_wait_policy(rp, RBIPC_WAIT_ADAPTIVE);
    rbipc_set_wait_policy(rp2, RBIPC_WAIT_ADAPTIVE);
    rbipc_set_wait_policy(rc, RBIPC_WAIT_ADAPTIVE);
    rbipc_set_wait_policy(rc2, RBIPC_WAIT_ADAPTIVE);

    uint64_t *samp = malloc(iters * sizeof(uint64_t));
    assert(samp != NULL);
    int sync_pipe[2];
    assert(pipe(sync_pipe) == 0);

    pid_t pid = fork();
    if (pid == 0) {
        close(sync_pipe[0]);
        ssize_t w = write(sync_pipe[1], "RDY", 3);
        (void)w;
        for (uint64_t n = 0; n < iters; n++) {
            const void *buf; uint64_t tk; uint32_t ln;
            while (rbipc_read_acquire(rc, &buf, &ln, &tk) != RBIPC_OK) {}
            rbipc_read_release(rc, tk);
            void *out; uint64_t t2;
            while (rbipc_reserve_write(rp2, 8, &out, &t2) != RBIPC_OK) {}
            rbipc_commit_write(rp2, t2, 8);
        }
        close(sync_pipe[1]);
        rbipc_detach(rc); rbipc_detach(rp2); _exit(0);
    } else {
        close(sync_pipe[1]);
        char rdy[4];
        ssize_t s = read(sync_pipe[0], rdy, 3);
        (void)s;
        close(sync_pipe[0]);

        for (uint64_t i = 0; i < iters; i++) {
            uint64_t t0 = comp_get_ns();
            void *buf; uint64_t tk;
            while (rbipc_reserve_write(rp, 8, &buf, &tk) != RBIPC_OK) {}
            rbipc_commit_write(rp, tk, 8);
            const void *rb; uint64_t rt; uint32_t rl;
            while (rbipc_read_acquire(rc2, &rb, &rl, &rt) != RBIPC_OK) {}
            rbipc_read_release(rc2, rt);
            samp[i] = (comp_get_ns() - t0) / 2;
        }
        uint64_t tot = 0;
        for (uint64_t i = 0; i < iters; i++) tot += samp[i];
        r.elapsed_ns = tot;
        qsort(samp, iters, sizeof(uint64_t), comp_cmp_u64);
        r.lat_min = samp[0]; r.lat_p50 = comp_pct64(samp, iters, 50.0);
        r.lat_p90 = comp_pct64(samp, iters, 90.0); r.lat_p99 = comp_pct64(samp, iters, 99.0);
        r.lat_p999 = comp_pct64(samp, iters, 99.9); r.lat_max = samp[iters - 1];
        r.tput_ops = (double)iters / ((double)tot * 2.0 / 1e9);
        waitpid(pid, NULL, 0);
    }
    rbipc_detach(rp); rbipc_detach(rc2);
    rbipc_destroy(sa); rbipc_destroy(sb);
    free(samp);
    return r;
}

result_t bench_rte_lat(uint64_t iters) {
    result_t r = {};
    snprintf(r.name, sizeof(r.name), "rte_ring (DPDK alg)");
    r.msgsz = 8; r.iters = iters;
    rte_ring_t *ra = rte_ring_alloc(256), *rb = rte_ring_alloc(256);
    uint64_t *samp = malloc(iters * sizeof(uint64_t));
    assert(samp != NULL);

    pid_t pid = fork();
    if (pid == 0) {
        uintptr_t v;
        for (uint64_t n = 0; n < iters; n++) {
            rte_dequeue_wait(ra, &v); rte_enqueue_wait(rb, v);
        }
        _exit(0);
    } else {
        uintptr_t d = 0xABCD, rv;
        for (uint64_t i = 0; i < iters; i++) {
            uint64_t t0 = comp_get_ns();
            rte_enqueue_wait(ra, d); rte_dequeue_wait(rb, &rv);
            samp[i] = (comp_get_ns() - t0) / 2;
        }
        uint64_t tot = 0;
        for (uint64_t i = 0; i < iters; i++) tot += samp[i];
        r.elapsed_ns = tot;
        qsort(samp, iters, sizeof(uint64_t), comp_cmp_u64);
        r.lat_min = samp[0]; r.lat_p50 = comp_pct64(samp, iters, 50.0);
        r.lat_p90 = comp_pct64(samp, iters, 90.0); r.lat_p99 = comp_pct64(samp, iters, 99.0);
        r.lat_p999 = comp_pct64(samp, iters, 99.9); r.lat_max = samp[iters - 1];
        r.tput_ops = (double)iters / ((double)tot * 2.0 / 1e9);
        waitpid(pid, NULL, 0);
    }
    rte_ring_free(ra); rte_ring_free(rb);
    free(samp);
    return r;
}

result_t bench_disruptor_lat(uint64_t iters) {
    result_t r = {};
    snprintf(r.name, sizeof(r.name), "LMAX Disruptor");
    r.msgsz = 8; r.iters = iters;
    disruptor_ring_t *da = disruptor_alloc(256), *db = disruptor_alloc(256);
    uint64_t *samp = malloc(iters * sizeof(uint64_t));
    assert(samp != NULL);

    pid_t pid = fork();
    if (pid == 0) {
        uintptr_t v;
        for (uint64_t n = 0; n < iters; n++) {
            while (!disruptor_consume_wp(da, (int64_t)iters, &v)) {
                __asm__ volatile("pause" ::: "memory");
            }
            disruptor_publish_sp(db, (int64_t)n, v);
        }
        _exit(0);
    } else {
        uintptr_t d = 0xABCD, rv;
        for (uint64_t i = 0; i < iters; i++) {
            uint64_t t0 = comp_get_ns();
            disruptor_publish_sp(da, (int64_t)i, d);
            while (!disruptor_consume_wp(db, (int64_t)iters, &rv)) {
                __asm__ volatile("pause" ::: "memory");
            }
            samp[i] = (comp_get_ns() - t0) / 2;
        }
        uint64_t tot = 0;
        for (uint64_t i = 0; i < iters; i++) tot += samp[i];
        r.elapsed_ns = tot;
        qsort(samp, iters, sizeof(uint64_t), comp_cmp_u64);
        r.lat_min = samp[0]; r.lat_p50 = comp_pct64(samp, iters, 50.0);
        r.lat_p90 = comp_pct64(samp, iters, 90.0); r.lat_p99 = comp_pct64(samp, iters, 99.0);
        r.lat_p999 = comp_pct64(samp, iters, 99.9); r.lat_max = samp[iters - 1];
        r.tput_ops = (double)iters / ((double)tot * 2.0 / 1e9);
        waitpid(pid, NULL, 0);
    }
    disruptor_free(da); disruptor_free(db);
    free(samp);
    return r;
}

result_t bench_mq_lat(uint64_t iters) {
    result_t r = {};
    snprintf(r.name, sizeof(r.name), "POSIX mqueue");
    r.msgsz = 8; r.iters = iters;
    mqd_t mab = mq_make(MQ_NAME_AB, 8, 10);
    mq_unlink(MQ_NAME_BA);
    mqd_t mba = mq_make(MQ_NAME_BA, 8, 10);
    uint64_t *samp = malloc(iters * sizeof(uint64_t));
    assert(samp != NULL);
    char buf[16]; unsigned int prio;

    pid_t pid = fork();
    if (pid == 0) {
        for (uint64_t n = 0; n < iters; n++) {
            if (mq_receive(mab, buf, 8, &prio) > 0) {
                mq_send(mba, buf, 8, MQ_PRIO);
            }
        }
        mq_close(mab); mq_close(mba); _exit(0);
    } else {
        char ping[8] = {};
        for (uint64_t i = 0; i < iters; i++) {
            uint64_t t0 = comp_get_ns();
            mq_send(mab, ping, 8, MQ_PRIO);
            mq_receive(mba, buf, 8, &prio);
            samp[i] = (comp_get_ns() - t0) / 2;
        }
        uint64_t tot = 0;
        for (uint64_t i = 0; i < iters; i++) tot += samp[i];
        r.elapsed_ns = tot;
        qsort(samp, iters, sizeof(uint64_t), comp_cmp_u64);
        r.lat_min = samp[0]; r.lat_p50 = comp_pct64(samp, iters, 50.0);
        r.lat_p90 = comp_pct64(samp, iters, 90.0); r.lat_p99 = comp_pct64(samp, iters, 99.0);
        r.lat_p999 = comp_pct64(samp, iters, 99.9); r.lat_max = samp[iters - 1];
        r.tput_ops = (double)iters / ((double)tot * 2.0 / 1e9);
        waitpid(pid, NULL, 0);
    }
    mq_close(mab); mq_close(mba); mq_unlink(MQ_NAME_AB); mq_unlink(MQ_NAME_BA);
    free(samp);
    return r;
}

result_t bench_uds_lat(uint64_t iters) {
    result_t r = {};
    snprintf(r.name, sizeof(r.name), "AF_UNIX DGRAM");
    r.msgsz = 8; r.iters = iters;
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0);
    uint64_t *samp = malloc(iters * sizeof(uint64_t));
    assert(samp != NULL);
    char buf[16]; char ping[8] = {};

    pid_t pid = fork();
    if (pid == 0) {
        close(sv[0]);
        for (uint64_t n = 0; n < iters; n++) {
            if (recv(sv[1], buf, 8, 0) > 0) {
                send(sv[1], buf, 8, 0);
            }
        }
        close(sv[1]);
        _exit(0);
    } else {
        close(sv[1]);
        for (uint64_t i = 0; i < iters; i++) {
            uint64_t t0 = comp_get_ns();
            send(sv[0], ping, 8, 0);
            recv(sv[0], buf, 8, 0);
            samp[i] = (comp_get_ns() - t0) / 2;
        }
        uint64_t tot = 0;
        for (uint64_t i = 0; i < iters; i++) tot += samp[i];
        r.elapsed_ns = tot;
        qsort(samp, iters, sizeof(uint64_t), comp_cmp_u64);
        r.lat_min = samp[0]; r.lat_p50 = comp_pct64(samp, iters, 50.0);
        r.lat_p90 = comp_pct64(samp, iters, 90.0); r.lat_p99 = comp_pct64(samp, iters, 99.0);
        r.lat_p999 = comp_pct64(samp, iters, 99.9); r.lat_max = samp[iters - 1];
        r.tput_ops = (double)iters / ((double)tot * 2.0 / 1e9);
        close(sv[0]);
        waitpid(pid, NULL, 0);
    }
    free(samp);
    return r;
}
