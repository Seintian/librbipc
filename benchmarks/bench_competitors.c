/**
 * @file bench_competitors.c
 * @brief Cross-System IPC Benchmark:
 *        librbipc vs. DPDK rte_ring, LMAX Disruptor, Linux POSIX mqueue, AF_UNIX DGRAM.
 *
 * Competing-Consumer Work Queue Paradigm:
 *  - Each message/event is delivered to and processed by EXACTLY ONE consumer.
 *  - Slots are immediately recycled upon release.
 *
 * Evaluated Systems:
 *  1. librbipc         - Mirrored shared memory + ticket-based reservation + futex passive wait.
 *  2. rte_ring         - DPDK lockless CAS-based MPMC ring buffer (standalone C11 atomics + shm).
 *                        Reference: ~/repos/dpdk/lib/ring/rte_ring_c11_pvt.h
 *  3. LMAX Disruptor   - Lock-free ring buffer with WorkerPool / WorkProcessor CAS sequence claiming.
 *                        Reference: ~/repos/Disruptor-cpp/Disruptor/WorkProcessor.h
 *                                   ~/repos/disruptor/src/main/java/com/lmax/disruptor/WorkProcessor.java
 *  4. POSIX mqueue     - Linux kernel-mediated message queues (mq_open/mq_send/mq_receive).
 *                        Reference: ~/repos/linux/tools/testing/selftests/mqueue/
 *  5. AF_UNIX DGRAM    - Linux Unix Domain Datagram sockets (socketpair(AF_UNIX, SOCK_DGRAM)).
 *                        Reference: ~/repos/linux/tools/testing/selftests/net/af_unix/
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/resource.h>
#include <mqueue.h>
#include <signal.h>
#include <sched.h>

#include "rbipc.h"
#include "bench_common.h"

/* ======================================================================
 * Constants & Configuration
 * ====================================================================== */

#define NSEC_PER_SEC        1000000000ULL
#define BENCH_ITERATIONS    200000ULL
#define WARMUP_ITERATIONS   2000ULL
#define LAT_ITERATIONS      30000ULL
#define RING_CAPACITY       4096U
#define CACHE_LINE_SIZE     64

#define COL_RESET   "\033[0m"
#define COL_BOLD    "\033[1m"
#define COL_RED     "\033[31m"
#define COL_GREEN   "\033[32m"
#define COL_YELLOW  "\033[33m"
#define COL_CYAN    "\033[36m"
#define COL_MAGENTA "\033[35m"

/* ======================================================================
 * Timing & Statistic Utilities
 * ====================================================================== */

static inline uint64_t get_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * NSEC_PER_SEC + (uint64_t)ts.tv_nsec;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static uint64_t pct64(uint64_t *arr, size_t n, double p) {
    if (!n) return 0;
    size_t i = (size_t)(p / 100.0 * (double)n);
    if (i >= n) i = n - 1;
    return arr[i];
}

static void print_sep(void) {
    printf("%s%s----------------------------------------------------------------------------------------%s\n",
           COL_BOLD, COL_CYAN, COL_RESET);
}

static void print_hdr(const char *t) {
    print_sep();
    printf("%s%s  %s%s\n", COL_BOLD, COL_YELLOW, t, COL_RESET);
    print_sep();
}

/* ======================================================================
 * SECTION 1: Standalone DPDK rte_ring
 * Reference: ~/repos/dpdk/lib/ring/rte_ring_c11_pvt.h
 * ====================================================================== */

typedef struct {
    alignas(CACHE_LINE_SIZE) atomic_uint prod_head;
    alignas(CACHE_LINE_SIZE) atomic_uint prod_tail;
    alignas(CACHE_LINE_SIZE) atomic_uint cons_head;
    alignas(CACHE_LINE_SIZE) atomic_uint cons_tail;
    uint32_t mask;
    uint32_t capacity;
} rte_ring_t;

static rte_ring_t *rte_ring_alloc(uint32_t cap) {
    assert((cap & (cap - 1)) == 0);
    size_t total = sizeof(rte_ring_t) + cap * sizeof(uintptr_t);
    void *mem = mmap(NULL, total, PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) return NULL;
    rte_ring_t *r = (rte_ring_t *)mem;
    atomic_init(&r->prod_head, 0); atomic_init(&r->prod_tail, 0);
    atomic_init(&r->cons_head, 0); atomic_init(&r->cons_tail, 0);
    r->mask = cap - 1; r->capacity = cap;
    return r;
}

static void rte_ring_free(rte_ring_t *r) {
    if (r) munmap(r, sizeof(rte_ring_t) + r->capacity * sizeof(uintptr_t));
}

static inline uintptr_t *rte_slots(rte_ring_t *r) {
    return (uintptr_t *)((char *)r + sizeof(rte_ring_t));
}

static int rte_mp_enqueue(rte_ring_t *r, uintptr_t val) {
    uint32_t ph, pn, ct;
    for (;;) {
        ph = atomic_load_explicit(&r->prod_head, memory_order_relaxed);
        ct = atomic_load_explicit(&r->cons_tail, memory_order_acquire);
        pn = ph + 1;
        if (r->mask + ct - ph == 0) return -ENOBUFS;
        if (atomic_compare_exchange_weak_explicit(&r->prod_head, &ph, pn,
                memory_order_acquire, memory_order_relaxed)) break;
    }
    rte_slots(r)[ph & r->mask] = val;
    uint32_t tail_spins = 0;
    while (atomic_load_explicit(&r->prod_tail, memory_order_relaxed) != ph)
        bench_spin_pause(&tail_spins);
    atomic_store_explicit(&r->prod_tail, pn, memory_order_release);
    return 0;
}

static int rte_mc_dequeue(rte_ring_t *r, uintptr_t *out) {
    uint32_t ch, cn, pt;
    for (;;) {
        ch = atomic_load_explicit(&r->cons_head, memory_order_relaxed);
        pt = atomic_load_explicit(&r->prod_tail, memory_order_acquire);
        if (ch == pt) return -ENOENT;
        cn = ch + 1;
        if (atomic_compare_exchange_weak_explicit(&r->cons_head, &ch, cn,
                memory_order_acquire, memory_order_relaxed)) break;
    }
    *out = rte_slots(r)[ch & r->mask];
    uint32_t cons_spins = 0;
    while (atomic_load_explicit(&r->cons_tail, memory_order_relaxed) != ch)
        bench_spin_pause(&cons_spins);
    atomic_store_explicit(&r->cons_tail, cn, memory_order_release);
    return 0;
}

static void rte_enqueue_wait(rte_ring_t *r, uintptr_t v) {
    uint32_t spins = 0;
    while (rte_mp_enqueue(r, v) == -ENOBUFS) bench_spin_pause(&spins);
}
static void rte_dequeue_wait(rte_ring_t *r, uintptr_t *v) {
    uint32_t spins = 0;
    while (rte_mc_dequeue(r, v) == -ENOENT) bench_spin_pause(&spins);
}

/* ======================================================================
 * SECTION 2: Standalone LMAX Disruptor (WorkProcessor / WorkerPool)
 * Reference: ~/repos/Disruptor-cpp/Disruptor/WorkProcessor.h
 *            ~/repos/disruptor/src/main/java/com/lmax/disruptor/WorkProcessor.java
 * ====================================================================== */

typedef struct {
    alignas(CACHE_LINE_SIZE) atomic_llong prod_cursor;
    alignas(CACHE_LINE_SIZE) atomic_llong prod_claim;
    alignas(CACHE_LINE_SIZE) atomic_llong work_sequence;
    alignas(CACHE_LINE_SIZE) atomic_llong cons_gating;
    uint32_t capacity;
    uint32_t mask;
} disruptor_ring_t;

static disruptor_ring_t *disruptor_alloc(uint32_t cap) {
    assert((cap & (cap - 1)) == 0);
    size_t sz = sizeof(disruptor_ring_t) + cap * sizeof(uintptr_t);
    void *m = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) return NULL;
    disruptor_ring_t *d = (disruptor_ring_t *)m;
    atomic_init(&d->prod_cursor, -1);
    atomic_init(&d->prod_claim, -1);
    atomic_init(&d->work_sequence, -1);
    atomic_init(&d->cons_gating, -1);
    d->capacity = cap;
    d->mask = cap - 1;
    return d;
}

static void disruptor_free(disruptor_ring_t *d) {
    if (d) munmap(d, sizeof(disruptor_ring_t) + d->capacity * sizeof(uintptr_t));
}

static inline uintptr_t *disruptor_slots(disruptor_ring_t *d) {
    return (uintptr_t *)((char *)d + sizeof(disruptor_ring_t));
}

/* Single-producer publishing */
static inline void disruptor_publish_sp(disruptor_ring_t *d, int64_t seq, uintptr_t val) {
    uint32_t spins = 0;
    while (seq - atomic_load_explicit(&d->cons_gating, memory_order_acquire) >= (int64_t)d->capacity) {
        bench_spin_pause(&spins);
    }
    disruptor_slots(d)[(uint32_t)seq & d->mask] = val;
    atomic_store_explicit(&d->prod_cursor, seq, memory_order_release);
}

/* Multi-producer publishing */
static inline void disruptor_publish_mp(disruptor_ring_t *d, uintptr_t val) {
    int64_t cur, next;
    uint32_t spins = 0;
    do {
        cur = atomic_load_explicit(&d->prod_claim, memory_order_relaxed);
        next = cur + 1;
        while (next - atomic_load_explicit(&d->cons_gating, memory_order_acquire) >= (int64_t)d->capacity) {
            bench_spin_pause(&spins);
        }
    } while (!atomic_compare_exchange_weak_explicit(&d->prod_claim, &cur, next,
                                                    memory_order_acq_rel, memory_order_relaxed));

    disruptor_slots(d)[(uint32_t)next & d->mask] = val;
    spins = 0;
    while (atomic_load_explicit(&d->prod_cursor, memory_order_acquire) != next - 1) {
        bench_spin_pause(&spins);
    }
    atomic_store_explicit(&d->prod_cursor, next, memory_order_release);
}

/* Competing-consumer WorkProcessor claim */
static inline bool disruptor_consume_wp(disruptor_ring_t *d, int64_t max_seq, uintptr_t *out) {
    int64_t cur, next;
    do {
        cur = atomic_load_explicit(&d->work_sequence, memory_order_relaxed);
        next = cur + 1;
        if (max_seq >= 0 && next >= max_seq) return false;
    } while (!atomic_compare_exchange_weak_explicit(&d->work_sequence, &cur, next,
                                                    memory_order_acq_rel, memory_order_relaxed));

    uint32_t spins = 0;
    while (atomic_load_explicit(&d->prod_cursor, memory_order_acquire) < next) {
        bench_spin_pause(&spins);
    }
    *out = disruptor_slots(d)[(uint32_t)next & d->mask];
    spins = 0;
    while (atomic_load_explicit(&d->cons_gating, memory_order_acquire) != next - 1) {
        bench_spin_pause(&spins);
    }
    atomic_store_explicit(&d->cons_gating, next, memory_order_release);
    return true;
}

/* ======================================================================
 * SECTION 3: POSIX mqueue wrappers
 * Reference: ~/repos/linux/tools/testing/selftests/mqueue/
 * ====================================================================== */

#define MQ_NAME_AB  "/rbench_mq_ab"
#define MQ_NAME_BA  "/rbench_mq_ba"
#define MQ_PRIO     1

static mqd_t mq_make(const char *name, size_t msgsz, long maxmsg) {
    long real_max = (maxmsg > 0 && maxmsg <= 10) ? maxmsg : 10;
    long real_sz  = (msgsz <= 8192) ? (long)msgsz : 8192;
    struct mq_attr a = { .mq_maxmsg = real_max, .mq_msgsize = real_sz, .mq_flags = 0, .mq_curmsgs = 0 };
    mq_unlink(name);
    mqd_t mq = mq_open(name, O_RDWR | O_CREAT | O_EXCL, 0660, &a);
    return mq;
}

/* ======================================================================
 * SECTION 4: Results Structure & Display
 * ====================================================================== */

typedef struct {
    char     name[48];
    size_t   msgsz;
    uint64_t iters;
    uint64_t elapsed_ns;
    double   tput_ops;
    double   tput_mbs;
    uint64_t lat_min;
    uint64_t lat_p50;
    uint64_t lat_p90;
    uint64_t lat_p99;
    uint64_t lat_p999;
    uint64_t lat_max;
} result_t;

static void print_result(const result_t *r) {
    printf("  %-25s | %10.2f Mops/s | %9.2f MB/s | Min: %4lu ns | p50: %5lu ns | p99: %6lu ns | p99.9: %7lu ns\n",
           r->name, r->tput_ops / 1e6, r->tput_mbs,
           (unsigned long)r->lat_min, (unsigned long)r->lat_p50,
           (unsigned long)r->lat_p99, (unsigned long)r->lat_p999);
}

static void print_table(result_t *arr, int n, const char *kind) {
    print_sep();
    printf("  %s%s Benchmark Summary (%s)%s\n", COL_BOLD, kind, (n > 0 ? arr[0].name : ""), COL_RESET);
    print_sep();
    printf("  %-25s  %15s  %13s  %10s  %10s  %10s  %10s\n",
           "System", "Throughput", "Bandwidth", "Min Lat", "p50 Lat", "p99 Lat", "p99.9 Lat");
    printf("  %-25s  %15s  %13s  %10s  %10s  %10s  %10s\n",
           "-------------------------", "---------------", "-------------",
           "----------", "----------", "----------", "----------");

    double max_tput = 0;
    for (int i = 0; i < n; i++) if (arr[i].tput_ops > max_tput) max_tput = arr[i].tput_ops;

    for (int i = 0; i < n; i++) {
        const char *col = (arr[i].tput_ops == max_tput) ? COL_GREEN : COL_RESET;
        printf("  %s%-25s%s  %s%12.2f Mops/s%s  %9.2f MB/s  %8lu ns  %8lu ns  %8lu ns  %8lu ns\n",
               col, arr[i].name, COL_RESET,
               col, arr[i].tput_ops / 1e6, COL_RESET,
               arr[i].tput_mbs,
               (unsigned long)arr[i].lat_min,
               (unsigned long)arr[i].lat_p50,
               (unsigned long)arr[i].lat_p99,
               (unsigned long)arr[i].lat_p999);
    }
    print_sep();
}

/* ======================================================================
 * SECTION 5: 1P-1C Throughput Benchmarks (Fork Model)
 * ====================================================================== */

static result_t bench_rbipc_tput(size_t msgsz, uint64_t iters) {
    result_t r = {0};
    snprintf(r.name, sizeof(r.name), "librbipc");
    r.msgsz = msgsz; r.iters = iters;

    const char *shm = "/rbench_rbipc_tput";
    rbipc_ring_t *rp = NULL, *rc = NULL;
    uint32_t slot = (uint32_t)(msgsz + 64);
    rbipc_destroy(shm);
    if (rbipc_create(shm, RING_CAPACITY, slot, &rp) != RBIPC_OK ||
        rbipc_attach(shm, &rc) != RBIPC_OK) return r;
    rbipc_set_wait_policy(rp, RBIPC_WAIT_ADAPTIVE);
    rbipc_set_wait_policy(rc, RBIPC_WAIT_ADAPTIVE);

    int sync_pipe[2];
    assert(pipe(sync_pipe) == 0);
    char *pay = aligned_alloc(64, msgsz > 64 ? msgsz : 64);
    memset(pay, 0xAB, msgsz);
    uint64_t t0 = 0, t1 = 0;

    pid_t pid = fork();
    if (pid == 0) {
        close(sync_pipe[0]);
        uint64_t n = 0;
        const void *buf; uint64_t tk; uint32_t ln;
        while (n < iters) {
            if (rbipc_read_acquire(rc, &buf, &ln, &tk) == RBIPC_OK) {
                rbipc_read_release(rc, tk);
                n++;
            }
        }
        char sig[4] = "OK!";
        write(sync_pipe[1], sig, sizeof(sig));
        close(sync_pipe[1]);
        rbipc_detach(rc);
        _exit(0);
    } else {
        close(sync_pipe[1]);
        t0 = get_ns();
        for (uint64_t i = 0; i < iters; i++) {
            void *buf; uint64_t tk;
            while (rbipc_reserve_write(rp, (uint32_t)msgsz, &buf, &tk) != RBIPC_OK) {
                __asm__ volatile("pause" ::: "memory");
            }
            memcpy(buf, pay, msgsz);
            rbipc_commit_write(rp, tk, (uint32_t)msgsz);
        }
        char sig[4];
        ssize_t s = read(sync_pipe[0], sig, sizeof(sig));
        (void)s;
        t1 = get_ns();
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

static result_t bench_rte_tput(size_t msgsz, uint64_t iters) {
    result_t r = {0};
    snprintf(r.name, sizeof(r.name), "rte_ring (DPDK alg)");
    r.msgsz = msgsz; r.iters = iters;

    size_t pool_sz = RING_CAPACITY * (msgsz > 64 ? msgsz : 64);
    char *pool = mmap(NULL, pool_sz, PROT_READ|PROT_WRITE,
                      MAP_SHARED|MAP_ANONYMOUS, -1, 0);
    memset(pool, 0xBC, pool_sz);
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
        char sig[4] = "OK!";
        write(sync_pipe[1], sig, sizeof(sig));
        close(sync_pipe[1]);
        _exit(0);
    } else {
        close(sync_pipe[1]);
        t0 = get_ns();
        for (uint64_t i = 0; i < iters; i++) {
            rte_enqueue_wait(ring, (uintptr_t)(pool + (i % RING_CAPACITY) * msgsz));
        }
        char sig[4];
        ssize_t s = read(sync_pipe[0], sig, sizeof(sig));
        (void)s;
        t1 = get_ns();
        close(sync_pipe[0]);
        waitpid(pid, NULL, 0);
    }

    rte_ring_free(ring);
    munmap(pool, pool_sz);
    r.elapsed_ns = t1 - t0;
    r.tput_ops   = (double)iters / ((double)r.elapsed_ns / 1e9);
    r.tput_mbs   = r.tput_ops * (double)msgsz / (1024.0 * 1024.0);
    return r;
}

static result_t bench_disruptor_tput(size_t msgsz, uint64_t iters) {
    result_t r = {0};
    snprintf(r.name, sizeof(r.name), "LMAX Disruptor");
    r.msgsz = msgsz; r.iters = iters;

    size_t pool_sz = RING_CAPACITY * (msgsz > 64 ? msgsz : 64);
    char *pool = mmap(NULL, pool_sz, PROT_READ|PROT_WRITE,
                      MAP_SHARED|MAP_ANONYMOUS, -1, 0);
    memset(pool, 0xAA, pool_sz);
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
        char sig[4] = "OK!";
        write(sync_pipe[1], sig, sizeof(sig));
        close(sync_pipe[1]);
        _exit(0);
    } else {
        close(sync_pipe[1]);
        t0 = get_ns();
        for (uint64_t i = 0; i < iters; i++) {
            uintptr_t ptr = (uintptr_t)(pool + (i % RING_CAPACITY) * msgsz);
            disruptor_publish_sp(ring, (int64_t)i, ptr);
        }
        char sig[4];
        ssize_t s = read(sync_pipe[0], sig, sizeof(sig));
        (void)s;
        t1 = get_ns();
        close(sync_pipe[0]);
        waitpid(pid, NULL, 0);
    }

    disruptor_free(ring);
    munmap(pool, pool_sz);
    r.elapsed_ns = t1 - t0;
    r.tput_ops   = (double)iters / ((double)r.elapsed_ns / 1e9);
    r.tput_mbs   = r.tput_ops * (double)msgsz / (1024.0 * 1024.0);
    return r;
}

static result_t bench_mq_tput(size_t msgsz, uint64_t iters) {
    result_t r = {0};
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
        char sig[4] = "OK!";
        write(sync_pipe[1], sig, sizeof(sig));
        close(sync_pipe[1]);
        mq_close(mq);
        _exit(0);
    } else {
        close(sync_pipe[1]);
        t0 = get_ns();
        for (uint64_t i = 0; i < iters; i++) {
            while (mq_send(mq, pay, msgsz, MQ_PRIO) < 0) {
                if (errno != EAGAIN && errno != EINTR) break;
            }
        }
        char sig[4];
        ssize_t s = read(sync_pipe[0], sig, sizeof(sig));
        (void)s;
        t1 = get_ns();
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

static result_t bench_uds_tput(size_t msgsz, uint64_t iters) {
    result_t r = {0};
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
        char sig[4] = "OK!";
        write(sync_pipe[1], sig, sizeof(sig));
        close(sync_pipe[1]);
        _exit(0);
    } else {
        close(sync_pipe[1]);
        close(sv[1]);
        t0 = get_ns();
        for (uint64_t i = 0; i < iters; i++) {
            ssize_t s = send(sv[0], pay, msgsz, 0);
            assert(s > 0);
        }
        char sig[4];
        ssize_t s = read(sync_pipe[0], sig, sizeof(sig));
        (void)s;
        t1 = get_ns();
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

/* ======================================================================
 * SECTION 6: Ping-Pong Half-RTT Latency Distribution Benchmarks
 * ====================================================================== */

static result_t bench_rbipc_lat(uint64_t iters) {
    result_t r = {0};
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
    int sync_pipe[2];
    assert(pipe(sync_pipe) == 0);

    pid_t pid = fork();
    if (pid == 0) {
        close(sync_pipe[0]);
        write(sync_pipe[1], "RDY", 3);
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
            uint64_t t0 = get_ns();
            void *buf; uint64_t tk;
            while (rbipc_reserve_write(rp, 8, &buf, &tk) != RBIPC_OK) {}
            rbipc_commit_write(rp, tk, 8);
            const void *rb; uint64_t rt; uint32_t rl;
            while (rbipc_read_acquire(rc2, &rb, &rl, &rt) != RBIPC_OK) {}
            rbipc_read_release(rc2, rt);
            samp[i] = (get_ns() - t0) / 2; /* half-RTT */
        }
        uint64_t tot = 0;
        for (uint64_t i = 0; i < iters; i++) tot += samp[i];
        r.elapsed_ns = tot;
        qsort(samp, iters, sizeof(uint64_t), cmp_u64);
        r.lat_min = samp[0]; r.lat_p50 = pct64(samp, iters, 50.0);
        r.lat_p90 = pct64(samp, iters, 90.0); r.lat_p99 = pct64(samp, iters, 99.0);
        r.lat_p999 = pct64(samp, iters, 99.9); r.lat_max = samp[iters - 1];
        r.tput_ops = (double)iters / ((double)tot * 2.0 / 1e9);
        waitpid(pid, NULL, 0);
    }
    rbipc_detach(rp); rbipc_detach(rc2);
    rbipc_destroy(sa); rbipc_destroy(sb);
    free(samp);
    return r;
}

static result_t bench_rte_lat(uint64_t iters) {
    result_t r = {0};
    snprintf(r.name, sizeof(r.name), "rte_ring (DPDK alg)");
    r.msgsz = 8; r.iters = iters;
    rte_ring_t *ra = rte_ring_alloc(256), *rb = rte_ring_alloc(256);
    uint64_t *samp = malloc(iters * sizeof(uint64_t));

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
            uint64_t t0 = get_ns();
            rte_enqueue_wait(ra, d); rte_dequeue_wait(rb, &rv);
            samp[i] = (get_ns() - t0) / 2;
        }
        uint64_t tot = 0;
        for (uint64_t i = 0; i < iters; i++) tot += samp[i];
        r.elapsed_ns = tot;
        qsort(samp, iters, sizeof(uint64_t), cmp_u64);
        r.lat_min = samp[0]; r.lat_p50 = pct64(samp, iters, 50.0);
        r.lat_p90 = pct64(samp, iters, 90.0); r.lat_p99 = pct64(samp, iters, 99.0);
        r.lat_p999 = pct64(samp, iters, 99.9); r.lat_max = samp[iters - 1];
        r.tput_ops = (double)iters / ((double)tot * 2.0 / 1e9);
        waitpid(pid, NULL, 0);
    }
    rte_ring_free(ra); rte_ring_free(rb);
    free(samp);
    return r;
}

static result_t bench_disruptor_lat(uint64_t iters) {
    result_t r = {0};
    snprintf(r.name, sizeof(r.name), "LMAX Disruptor");
    r.msgsz = 8; r.iters = iters;
    disruptor_ring_t *da = disruptor_alloc(256), *db = disruptor_alloc(256);
    uint64_t *samp = malloc(iters * sizeof(uint64_t));

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
            uint64_t t0 = get_ns();
            disruptor_publish_sp(da, (int64_t)i, d);
            while (!disruptor_consume_wp(db, (int64_t)iters, &rv)) {
                __asm__ volatile("pause" ::: "memory");
            }
            samp[i] = (get_ns() - t0) / 2;
        }
        uint64_t tot = 0;
        for (uint64_t i = 0; i < iters; i++) tot += samp[i];
        r.elapsed_ns = tot;
        qsort(samp, iters, sizeof(uint64_t), cmp_u64);
        r.lat_min = samp[0]; r.lat_p50 = pct64(samp, iters, 50.0);
        r.lat_p90 = pct64(samp, iters, 90.0); r.lat_p99 = pct64(samp, iters, 99.0);
        r.lat_p999 = pct64(samp, iters, 99.9); r.lat_max = samp[iters - 1];
        r.tput_ops = (double)iters / ((double)tot * 2.0 / 1e9);
        waitpid(pid, NULL, 0);
    }
    disruptor_free(da); disruptor_free(db);
    free(samp);
    return r;
}

static result_t bench_mq_lat(uint64_t iters) {
    result_t r = {0};
    snprintf(r.name, sizeof(r.name), "POSIX mqueue");
    r.msgsz = 8; r.iters = iters;
    mqd_t mab = mq_make(MQ_NAME_AB, 8, 10);
    mq_unlink(MQ_NAME_BA);
    mqd_t mba = mq_make(MQ_NAME_BA, 8, 10);
    uint64_t *samp = malloc(iters * sizeof(uint64_t));
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
        char ping[8] = {0};
        for (uint64_t i = 0; i < iters; i++) {
            uint64_t t0 = get_ns();
            mq_send(mab, ping, 8, MQ_PRIO);
            mq_receive(mba, buf, 8, &prio);
            samp[i] = (get_ns() - t0) / 2;
        }
        uint64_t tot = 0;
        for (uint64_t i = 0; i < iters; i++) tot += samp[i];
        r.elapsed_ns = tot;
        qsort(samp, iters, sizeof(uint64_t), cmp_u64);
        r.lat_min = samp[0]; r.lat_p50 = pct64(samp, iters, 50.0);
        r.lat_p90 = pct64(samp, iters, 90.0); r.lat_p99 = pct64(samp, iters, 99.0);
        r.lat_p999 = pct64(samp, iters, 99.9); r.lat_max = samp[iters - 1];
        r.tput_ops = (double)iters / ((double)tot * 2.0 / 1e9);
        waitpid(pid, NULL, 0);
    }
    mq_close(mab); mq_close(mba); mq_unlink(MQ_NAME_AB); mq_unlink(MQ_NAME_BA);
    free(samp);
    return r;
}

static result_t bench_uds_lat(uint64_t iters) {
    result_t r = {0};
    snprintf(r.name, sizeof(r.name), "AF_UNIX DGRAM");
    r.msgsz = 8; r.iters = iters;
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0);
    uint64_t *samp = malloc(iters * sizeof(uint64_t));
    char buf[16]; char ping[8] = {0};

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
            uint64_t t0 = get_ns();
            send(sv[0], ping, 8, 0);
            recv(sv[0], buf, 8, 0);
            samp[i] = (get_ns() - t0) / 2;
        }
        uint64_t tot = 0;
        for (uint64_t i = 0; i < iters; i++) tot += samp[i];
        r.elapsed_ns = tot;
        qsort(samp, iters, sizeof(uint64_t), cmp_u64);
        r.lat_min = samp[0]; r.lat_p50 = pct64(samp, iters, 50.0);
        r.lat_p90 = pct64(samp, iters, 90.0); r.lat_p99 = pct64(samp, iters, 99.0);
        r.lat_p999 = pct64(samp, iters, 99.9); r.lat_max = samp[iters - 1];
        r.tput_ops = (double)iters / ((double)tot * 2.0 / 1e9);
        close(sv[0]);
        waitpid(pid, NULL, 0);
    }
    free(samp);
    return r;
}

/* ======================================================================
 * SECTION 7: Payload-Size Scaling
 * ====================================================================== */

static const struct { size_t sz; const char *label; } psizes[] = {
    {64,    "  64 B"}, {256,   " 256 B"}, {1024,  "  1 KB"},
    {4096,  "  4 KB"}, {16384, " 16 KB"}, {65536, " 64 KB"},
};
#define NPSIZES ((int)(sizeof(psizes)/sizeof(psizes[0])))

/* ======================================================================
 * SECTION 8: MPMC Scalability (Intra-Process Multi-Threading)
 * ====================================================================== */

/* --- rte_ring MPMC --- */
typedef struct {
    rte_ring_t *ring;
    uint64_t    iters;
    int         n_prod, n_cons;
} rte_mc_t;

static void *rte_prod_th(void *a) {
    rte_mc_t *c = a;
    uintptr_t v = 0x1234;
    for (uint64_t i = 0; i < c->iters; i++) rte_enqueue_wait(c->ring, v);
    return NULL;
}
static void *rte_cons_th(void *a) {
    rte_mc_t *c = a;
    uint64_t tgt = (uint64_t)c->n_prod * c->iters / (uint64_t)c->n_cons;
    uint64_t n = 0; uintptr_t v;
    uint32_t spins = 0;
    while (n < tgt) {
        if (rte_mc_dequeue(c->ring, &v) == 0) {
            n++;
            spins = 0;
        } else {
            bench_spin_pause(&spins);
        }
    }
    return NULL;
}

static double bench_rte_mpmc(int np, int nc, uint64_t iters_p) {
    rte_mc_t c = { .ring = rte_ring_alloc(RING_CAPACITY), .iters = iters_p, .n_prod = np, .n_cons = nc };
    pthread_t *pt = malloc((size_t)np * sizeof(pthread_t));
    pthread_t *ct = malloc((size_t)nc * sizeof(pthread_t));
    uint64_t t0 = get_ns();
    for (int i = 0; i < nc; i++) pthread_create(&ct[i], NULL, rte_cons_th, &c);
    for (int i = 0; i < np; i++) pthread_create(&pt[i], NULL, rte_prod_th, &c);
    for (int i = 0; i < np; i++) pthread_join(pt[i], NULL);
    for (int i = 0; i < nc; i++) pthread_join(ct[i], NULL);
    uint64_t t1 = get_ns();
    double r = (double)((uint64_t)np * iters_p) / ((double)(t1 - t0) / 1e9);
    rte_ring_free(c.ring); free(pt); free(ct);
    return r;
}

/* --- LMAX Disruptor MPMC --- */
typedef struct {
    disruptor_ring_t *ring;
    uint64_t          iters;
    int64_t           total_msgs;
} disr_mc_t;

static void *disr_prod_th(void *a) {
    disr_mc_t *c = a;
    uintptr_t v = 0x5678;
    for (uint64_t i = 0; i < c->iters; i++) disruptor_publish_mp(c->ring, v);
    return NULL;
}
static void *disr_cons_th(void *a) {
    disr_mc_t *c = a;
    uintptr_t v;
    while (disruptor_consume_wp(c->ring, c->total_msgs, &v)) {}
    return NULL;
}

static double bench_disruptor_mpmc(int np, int nc, uint64_t iters_p) {
    disr_mc_t c = {
        .ring = disruptor_alloc(RING_CAPACITY),
        .iters = iters_p,
        .total_msgs = (int64_t)((uint64_t)np * iters_p)
    };
    pthread_t *pt = malloc((size_t)np * sizeof(pthread_t));
    pthread_t *ct = malloc((size_t)nc * sizeof(pthread_t));
    uint64_t t0 = get_ns();
    for (int i = 0; i < nc; i++) pthread_create(&ct[i], NULL, disr_cons_th, &c);
    for (int i = 0; i < np; i++) pthread_create(&pt[i], NULL, disr_prod_th, &c);
    for (int i = 0; i < np; i++) pthread_join(pt[i], NULL);
    for (int i = 0; i < nc; i++) pthread_join(ct[i], NULL);
    uint64_t t1 = get_ns();
    double r = (double)((uint64_t)np * iters_p) / ((double)(t1 - t0) / 1e9);
    disruptor_free(c.ring); free(pt); free(ct);
    return r;
}

/* --- librbipc MPMC --- */
typedef struct {
    rbipc_ring_t *ring;
    uint64_t      iters;
    size_t        msgsz;
    atomic_ullong recv;
    uint64_t      total_msgs;
} rbi_mc_t;

static void *rbi_prod_th(void *a) {
    rbi_mc_t *c = a;
    char *pay = malloc(c->msgsz); memset(pay, 0xAB, c->msgsz);
    for (uint64_t i = 0; i < c->iters; i++) {
        void *buf; uint64_t tk;
        uint32_t spins = 0;
        while (rbipc_reserve_write(c->ring, (uint32_t)c->msgsz, &buf, &tk) != RBIPC_OK)
            bench_spin_pause(&spins);
        memcpy(buf, pay, c->msgsz);
        rbipc_commit_write(c->ring, tk, (uint32_t)c->msgsz);
    }
    free(pay);
    return NULL;
}
static void *rbi_cons_th(void *a) {
    rbi_mc_t *c = a;
    uint32_t spins = 0;
    while (atomic_load(&c->recv) < c->total_msgs) {
        const void *buf; uint64_t tk; uint32_t ln;
        if (rbipc_read_acquire_nonblock(c->ring, &buf, &ln, &tk) == RBIPC_OK) {
            rbipc_read_release(c->ring, tk);
            atomic_fetch_add(&c->recv, 1);
            spins = 0;
        } else {
            bench_spin_pause(&spins);
        }
    }
    return NULL;
}

static double bench_rbi_mpmc(int np, int nc, uint64_t iters_p, rbipc_ring_t *ring, size_t msgsz) {
    rbi_mc_t c = {
        .ring = ring,
        .iters = iters_p,
        .msgsz = msgsz,
        .total_msgs = (uint64_t)np * iters_p,
        .recv = 0
    };
    atomic_init(&c.recv, 0);
    pthread_t *pt = malloc((size_t)np * sizeof(pthread_t));
    pthread_t *ct = malloc((size_t)nc * sizeof(pthread_t));
    uint64_t t0 = get_ns();
    for (int i = 0; i < nc; i++) pthread_create(&ct[i], NULL, rbi_cons_th, &c);
    for (int i = 0; i < np; i++) pthread_create(&pt[i], NULL, rbi_prod_th, &c);
    for (int i = 0; i < np; i++) pthread_join(pt[i], NULL);
    for (int i = 0; i < nc; i++) pthread_join(ct[i], NULL);
    uint64_t t1 = get_ns();
    double r = (double)((uint64_t)np * iters_p) / ((double)(t1 - t0) / 1e9);
    free(pt); free(ct);
    return r;
}

/* ======================================================================
 * SECTION 9: Idle CPU Utilisation
 * ====================================================================== */

static double idle_cpu(struct rusage *r0, struct rusage *r1, uint64_t elapsed_ns) {
    uint64_t cpu_us =
        (uint64_t)(r1->ru_utime.tv_sec  - r0->ru_utime.tv_sec)  * 1000000u +
        (uint64_t)(r1->ru_utime.tv_usec - r0->ru_utime.tv_usec) +
        (uint64_t)(r1->ru_stime.tv_sec  - r0->ru_stime.tv_sec)  * 1000000u +
        (uint64_t)(r1->ru_stime.tv_usec - r0->ru_stime.tv_usec);
    return (double)cpu_us / ((double)elapsed_ns / 1000.0) * 100.0;
}

/* ======================================================================
 * MAIN BENCHMARK DRIVER
 * ====================================================================== */

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    /* Clean up any leftover artifacts */
    rbipc_destroy("/rbench_rbipc_tput");
    rbipc_destroy("/rbench_lat_a");
    rbipc_destroy("/rbench_lat_b");
    rbipc_destroy("/rbench_batch");
    rbipc_destroy("/rbench_mpmc");
    rbipc_destroy("/rbench_idle");
    mq_unlink(MQ_NAME_AB); mq_unlink(MQ_NAME_BA);

    printf("\n");
    printf("%s%s=== librbipc vs. DPDK rte_ring | LMAX Disruptor | POSIX mqueue | AF_UNIX DGRAM ===%s\n",
           COL_BOLD, COL_CYAN, COL_RESET);
    printf("%s%s    Exhaustive Cross-System Competing-Consumer IPC Benchmark%s\n", COL_BOLD, COL_CYAN, COL_RESET);
    printf("%s%s    Source Repos: ~/repos/{dpdk, linux, Disruptor-cpp, disruptor, librbipc}%s\n\n", COL_BOLD, COL_CYAN, COL_RESET);

    char json_path[256];
    bench_get_result_path(json_path, sizeof(json_path), "competitors_benchmark.json");
    FILE *json_fp = fopen(json_path, "w");
    if (json_fp) {
        bench_hardware_info_t hw;
        bench_get_hardware_info(&hw);
        fprintf(json_fp, "{\n  \"timestamp\": %lu,\n", (unsigned long)time(NULL));
        fprintf(json_fp, "  \"hardware\": {\n");
        fprintf(json_fp, "    \"profile\": \"%s\",\n", hw.profile_name);
        fprintf(json_fp, "    \"cpu\": \"%s\",\n", hw.cpu_model);
        fprintf(json_fp, "    \"cores\": %d,\n    \"threads\": %d,\n", hw.physical_cores, hw.logical_threads);
        fprintf(json_fp, "    \"hostname\": \"%s\",\n", hw.hostname);
        fprintf(json_fp, "    \"kernel\": \"%s\"\n  },\n", hw.kernel);
    }

    uint64_t bench_iters = bench_scale_iterations(BENCH_ITERATIONS, 100000ULL);
    uint64_t lat_iters = bench_scale_iterations(LAT_ITERATIONS, 15000ULL);
    uint64_t payload_iters = bench_scale_iterations(50000ULL, 20000ULL);
    uint64_t mpmc_iters = bench_scale_iterations(100000ULL, 25000ULL);
    uint64_t batch_iters = bench_scale_iterations(200000ULL, 100000ULL);

    /* -----------------------------------------------------------------
     * BENCH 1: 1P-1C Throughput (64 B)
     * ----------------------------------------------------------------- */
    char hdr_buf[128];
    snprintf(hdr_buf, sizeof(hdr_buf), "BENCH 1: 1P-1C Throughput -- 64 B Payloads, %lu Iterations", (unsigned long)bench_iters);
    print_hdr(hdr_buf);

    result_t tput[5];
    printf("  [1/5] librbipc ...\n");
    tput[0] = bench_rbipc_tput(64, bench_iters);
    printf("  [2/5] rte_ring (DPDK algorithm) ...\n");
    tput[1] = bench_rte_tput(64, bench_iters);
    printf("  [3/5] LMAX Disruptor (WorkProcessor) ...\n");
    tput[2] = bench_disruptor_tput(64, bench_iters);
    printf("  [4/5] POSIX mqueue ...\n");
    tput[3] = bench_mq_tput(64, bench_iters);
    printf("  [5/5] AF_UNIX SOCK_DGRAM ...\n");
    tput[4] = bench_uds_tput(64, bench_iters);

    for (int i = 0; i < 5; i++) { printf("\n"); print_result(&tput[i]); }
    print_table(tput, 5, "1P-1C Streaming Throughput");

    if (json_fp) {
        fprintf(json_fp, "  \"throughput_64b\": [\n");
        for (int i = 0; i < 5; i++) {
            fprintf(json_fp, "    {\"name\": \"%s\", \"ops_sec\": %.2f, \"mb_sec\": %.2f}%s\n",
                    tput[i].name, tput[i].tput_ops, tput[i].tput_mbs, (i == 4 ? "" : ","));
        }
        fprintf(json_fp, "  ],\n");
    }

    /* -----------------------------------------------------------------
     * BENCH 2: Half-RTT Ping-Pong Latency Distribution
     * ----------------------------------------------------------------- */
    snprintf(hdr_buf, sizeof(hdr_buf), "BENCH 2: Half-RTT Ping-Pong Latency Distribution -- %lu Iterations", (unsigned long)lat_iters);
    print_hdr(hdr_buf);

    result_t lat[5];
    printf("  [1/5] librbipc latency ...\n");
    lat[0] = bench_rbipc_lat(lat_iters);
    printf("  [2/5] rte_ring latency ...\n");
    lat[1] = bench_rte_lat(lat_iters);
    printf("  [3/5] LMAX Disruptor latency ...\n");
    lat[2] = bench_disruptor_lat(lat_iters);
    printf("  [4/5] POSIX mqueue latency ...\n");
    lat[3] = bench_mq_lat(lat_iters);
    printf("  [5/5] AF_UNIX DGRAM latency ...\n");
    lat[4] = bench_uds_lat(lat_iters);

    for (int i = 0; i < 5; i++) { printf("\n"); print_result(&lat[i]); }
    print_table(lat, 5, "Half-RTT Latency Distribution");

    if (json_fp) {
        fprintf(json_fp, "  \"latency_distribution\": [\n");
        for (int i = 0; i < 5; i++) {
            fprintf(json_fp, "    {\"name\": \"%s\", \"min_ns\": %lu, \"p50_ns\": %lu, \"p90_ns\": %lu, \"p99_ns\": %lu, \"p999_ns\": %lu, \"max_ns\": %lu}%s\n",
                    lat[i].name, (unsigned long)lat[i].lat_min, (unsigned long)lat[i].lat_p50,
                    (unsigned long)lat[i].lat_p90, (unsigned long)lat[i].lat_p99,
                    (unsigned long)lat[i].lat_p999, (unsigned long)lat[i].lat_max,
                    (i == 4 ? "" : ","));
        }
        fprintf(json_fp, "  ],\n");
    }

    /* -----------------------------------------------------------------
     * BENCH 3: Payload-Size Scaling
     * ----------------------------------------------------------------- */
    snprintf(hdr_buf, sizeof(hdr_buf), "BENCH 3: Payload-Size Scaling -- %lu Iterations per Size", (unsigned long)payload_iters);
    print_hdr(hdr_buf);

    printf("\n  %-8s  %s%16s%s  %16s  %16s  %16s  %16s\n",
           "Size", COL_BOLD, "librbipc", COL_RESET,
           "rte_ring", "Disruptor", "POSIX mqueue", "AF_UNIX DGRAM");
    printf("  %-8s  %16s  %16s  %16s  %16s  %16s\n",
           "--------", "----------------", "----------------", "----------------",
           "----------------", "----------------");

    if (json_fp) fprintf(json_fp, "  \"payload_scaling\": [\n");

    for (int s = 0; s < NPSIZES; s++) {
        size_t sz = psizes[s].sz;
        result_t rb = bench_rbipc_tput(sz, payload_iters);
        result_t rt = bench_rte_tput(sz, payload_iters);
        result_t ds = bench_disruptor_tput(sz, payload_iters);
        result_t mq = (sz <= 8192) ? bench_mq_tput(sz, payload_iters) : (result_t){.name="POSIX mqueue", .tput_ops=0};
        result_t ud = bench_uds_tput(sz, payload_iters);

        printf("  %-8s  %15.2fM  %15.2fM  %15.2fM  %15.2fM  %15.2fM\n",
               psizes[s].label,
               rb.tput_ops / 1e6, rt.tput_ops / 1e6, ds.tput_ops / 1e6,
               mq.tput_ops / 1e6, ud.tput_ops / 1e6);

        if (json_fp) {
            fprintf(json_fp, "    {\"size\": %zu, \"librbipc\": %.2f, \"rte_ring\": %.2f, \"disruptor\": %.2f, \"mqueue\": %.2f, \"af_unix\": %.2f}%s\n",
                    sz, rb.tput_ops, rt.tput_ops, ds.tput_ops, mq.tput_ops, ud.tput_ops,
                    (s == NPSIZES - 1 ? "" : ","));
        }
    }
    if (json_fp) fprintf(json_fp, "  ],\n");

    /* -----------------------------------------------------------------
     * BENCH 4: Idle CPU Consumption (400ms sampling window)
     * ----------------------------------------------------------------- */
    print_hdr("BENCH 4: Idle Consumer CPU Utilisation -- 400ms Window (Passive vs Busy)");

    const uint64_t IDLE_NS = 400000000ULL;
    double cpu_rte = 0, cpu_dis = 0, cpu_mq = 0, cpu_uds = 0, cpu_rbi = 0;

    /* rte_ring: busy-poll */
    {
        rte_ring_t *idle_r = rte_ring_alloc(RING_CAPACITY);
        struct rusage ru0, ru1;
        getrusage(RUSAGE_SELF, &ru0);
        uint64_t t0 = get_ns();
        uintptr_t v;
        while (get_ns() - t0 < IDLE_NS) rte_mc_dequeue(idle_r, &v);
        getrusage(RUSAGE_SELF, &ru1);
        cpu_rte = idle_cpu(&ru0, &ru1, get_ns() - t0);
        printf("\n  %-25s  CPU Idle: %s%5.1f%%%s  (busy-spin -- DPDK poll-mode)\n",
               "rte_ring (DPDK alg)", COL_RED, cpu_rte, COL_RESET);
        rte_ring_free(idle_r);
    }

    /* Disruptor: busy-poll */
    {
        disruptor_ring_t *idle_d = disruptor_alloc(RING_CAPACITY);
        struct rusage ru0, ru1;
        getrusage(RUSAGE_SELF, &ru0);
        uint64_t t0 = get_ns();
        uintptr_t v;
        while (get_ns() - t0 < IDLE_NS) disruptor_consume_wp(idle_d, 0, &v);
        getrusage(RUSAGE_SELF, &ru1);
        cpu_dis = idle_cpu(&ru0, &ru1, get_ns() - t0);
        printf("  %-25s  CPU Idle: %s%5.1f%%%s  (busy-spin -- sequence gating)\n",
               "LMAX Disruptor", COL_RED, cpu_dis, COL_RESET);
        disruptor_free(idle_d);
    }

    /* POSIX mqueue: non-blocking spin */
    {
        mqd_t imq = mq_make(MQ_NAME_AB, 8, 10);
        struct mq_attr a = { .mq_flags = O_NONBLOCK, .mq_maxmsg = 10, .mq_msgsize = 8, .mq_curmsgs = 0 };
        mq_setattr(imq, &a, NULL);
        struct rusage ru0, ru1;
        getrusage(RUSAGE_SELF, &ru0);
        uint64_t t0 = get_ns();
        char buf[16]; unsigned int p;
        while (get_ns() - t0 < IDLE_NS) mq_receive(imq, buf, 8, &p);
        getrusage(RUSAGE_SELF, &ru1);
        cpu_mq = idle_cpu(&ru0, &ru1, get_ns() - t0);
        printf("  %-25s  CPU Idle: %s%5.1f%%%s  (non-blocking spin)\n",
               "POSIX mqueue", COL_YELLOW, cpu_mq, COL_RESET);
        mq_close(imq); mq_unlink(MQ_NAME_AB);
    }

    /* AF_UNIX: non-blocking recv */
    {
        int sv[2];
        assert(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0);
        int fl = fcntl(sv[1], F_GETFL);
        fcntl(sv[1], F_SETFL, fl | O_NONBLOCK);
        struct rusage ru0, ru1;
        getrusage(RUSAGE_SELF, &ru0);
        uint64_t t0 = get_ns();
        char buf[16];
        while (get_ns() - t0 < IDLE_NS) recv(sv[1], buf, 8, 0);
        getrusage(RUSAGE_SELF, &ru1);
        cpu_uds = idle_cpu(&ru0, &ru1, get_ns() - t0);
        printf("  %-25s  CPU Idle: %s%5.1f%%%s  (non-blocking recv)\n",
               "AF_UNIX DGRAM", COL_YELLOW, cpu_uds, COL_RESET);
        close(sv[0]); close(sv[1]);
    }

    /* librbipc: futex passive wait */
    {
        rbipc_ring_t *ir = NULL;
        rbipc_destroy("/rbench_idle");
        if (rbipc_create("/rbench_idle", RING_CAPACITY, 72, &ir) == RBIPC_OK) {
            struct rusage ru0, ru1;
            getrusage(RUSAGE_SELF, &ru0);
            uint64_t t0 = get_ns();
            while (get_ns() - t0 < IDLE_NS) {
                const void *buf; uint64_t tk; uint32_t ln;
                int rto = rbipc_read_acquire_timeout(ir, 10000000ULL, &buf, &ln, &tk);
                (void)rto;
            }
            getrusage(RUSAGE_SELF, &ru1);
            cpu_rbi = idle_cpu(&ru0, &ru1, get_ns() - t0);
            printf("  %-25s  CPU Idle: %s%5.1f%%%s  (futex passive sleep)\n",
               "librbipc", COL_GREEN, cpu_rbi, COL_RESET);
            rbipc_detach(ir);
            rbipc_destroy("/rbench_idle");
        }
    }

    if (json_fp) {
        fprintf(json_fp, "  \"idle_cpu\": {\n");
        fprintf(json_fp, "    \"rte_ring\": %.1f,\n    \"disruptor\": %.1f,\n    \"mqueue\": %.1f,\n    \"af_unix\": %.1f,\n    \"librbipc\": %.1f\n",
                cpu_rte, cpu_dis, cpu_mq, cpu_uds, cpu_rbi);
        fprintf(json_fp, "  },\n");
    }

    /* -----------------------------------------------------------------
     * BENCH 5: MPMC Thread Scalability
     * ----------------------------------------------------------------- */
    snprintf(hdr_buf, sizeof(hdr_buf), "BENCH 5: MPMC Thread Scalability -- %lu Iterations / Producer", (unsigned long)mpmc_iters);
    print_hdr(hdr_buf);

    rbipc_ring_t *mpmc_r = NULL;
    rbipc_destroy("/rbench_mpmc");
    if (rbipc_create("/rbench_mpmc", RING_CAPACITY, 128, &mpmc_r) == RBIPC_OK) {
        rbipc_set_wait_policy(mpmc_r, RBIPC_WAIT_ADAPTIVE);
        typedef struct { int np, nc; } cfg_t;
        cfg_t cfgs[] = { {1,1}, {2,2}, {4,2}, {4,4} };
        int ncfgs = (int)(sizeof(cfgs)/sizeof(cfgs[0]));

        printf("\n  %s%-10s  %18s  %18s  %18s%s\n",
               COL_BOLD, "Config", "librbipc (Mops/s)", "rte_ring (Mops/s)", "Disruptor (Mops/s)", COL_RESET);
        printf("  %-10s  %18s  %18s  %18s\n", "----------", "------------------", "------------------", "------------------");

        if (json_fp) fprintf(json_fp, "  \"mpmc_scalability\": [\n");

        for (int i = 0; i < ncfgs; i++) {
            double rbi = bench_rbi_mpmc(cfgs[i].np, cfgs[i].nc, mpmc_iters, mpmc_r, 64);
            double rte = bench_rte_mpmc(cfgs[i].np, cfgs[i].nc, mpmc_iters);
            double dis = bench_disruptor_mpmc(cfgs[i].np, cfgs[i].nc, mpmc_iters);

            printf("  %dP-%-6dC  %18.2f  %18.2f  %18.2f\n",
                   cfgs[i].np, cfgs[i].nc,
                   rbi / 1e6, rte / 1e6, dis / 1e6);

            if (json_fp) {
                fprintf(json_fp, "    {\"config\": \"%dP-%dC\", \"librbipc\": %.2f, \"rte_ring\": %.2f, \"disruptor\": %.2f}%s\n",
                        cfgs[i].np, cfgs[i].nc, rbi, rte, dis, (i == ncfgs - 1 ? "" : ","));
            }
        }
        rbipc_detach(mpmc_r);
        rbipc_destroy("/rbench_mpmc");
        if (json_fp) fprintf(json_fp, "  ],\n");
    }

    /* -----------------------------------------------------------------
     * BENCH 6: librbipc Batch API Scaling
     * ----------------------------------------------------------------- */
    snprintf(hdr_buf, sizeof(hdr_buf), "BENCH 6: librbipc Vector/Batch Enqueue Scaling -- %lu Ops", (unsigned long)batch_iters);
    print_hdr(hdr_buf);

    int bsizes[] = {1, 4, 8, 16, 32, 64};
    int nb = (int)(sizeof(bsizes)/sizeof(bsizes[0]));

    printf("\n  %s%-12s  %20s%s\n", COL_BOLD, "Batch Size", "librbipc (Mops/s)", COL_RESET);
    printf("  %-12s  %20s\n", "------------", "--------------------");

    if (json_fp) fprintf(json_fp, "  \"batch_scaling\": [\n");

    for (int b = 0; b < nb; b++) {
        uint32_t batch = (uint32_t)bsizes[b];
        rbipc_ring_t *br = NULL;
        rbipc_destroy("/rbench_batch");
        if (rbipc_create("/rbench_batch", RING_CAPACITY, 128, &br) == RBIPC_OK) {
            rbipc_set_wait_policy(br, RBIPC_WAIT_ADAPTIVE);
            uint64_t total = batch_iters, iters = total / batch;
            uint64_t t0 = get_ns();
            rbipc_iovec_t iovecs[64];
            uint64_t tickets[64];
            uint32_t lens[64];

            for (uint64_t i = 0; i < iters; i++) {
                uint32_t reserved = 0;
                uint32_t spins = 0;
                while (rbipc_reserve_write_batch(br, batch, iovecs, &reserved) != RBIPC_OK)
                    bench_spin_pause(&spins);
                for (uint32_t k = 0; k < reserved; k++) {
                    tickets[k] = iovecs[k].ticket;
                    lens[k] = 64;
                }
                rbipc_commit_write_batch(br, reserved, tickets, lens);

                for (uint32_t j = 0; j < reserved; j++) {
                    const void *rb; uint64_t rt; uint32_t rl;
                    spins = 0;
                    while (rbipc_read_acquire_nonblock(br, &rb, &rl, &rt) != RBIPC_OK)
                        bench_spin_pause(&spins);
                    rbipc_read_release(br, rt);
                }
            }
            uint64_t t1 = get_ns();
            double tput_val = (double)total / ((double)(t1 - t0) / 1e9);
            printf("  %-12u  %20.2f\n", batch, tput_val / 1e6);

            if (json_fp) {
                fprintf(json_fp, "    {\"batch_size\": %u, \"ops_sec\": %.2f}%s\n",
                        batch, tput_val, (b == nb - 1 ? "" : ","));
            }
            rbipc_detach(br);
            rbipc_destroy("/rbench_batch");
        }
    }
    if (json_fp) {
        fprintf(json_fp, "  ]\n}\n");
        fclose(json_fp);
    }

    print_sep();
    printf("  Benchmark complete. JSON results exported to %s\n", json_path);
    print_sep();

    return 0;
}
