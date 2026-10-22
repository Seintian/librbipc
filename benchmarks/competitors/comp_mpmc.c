/**
 * @file comp_mpmc.c
 * @brief MPMC thread scalability benchmark for competitors under identical memory constraints.
 */

#include "comp_common.h"

/* --- rte_ring MPMC --- */
typedef struct {
    rte_ring_t *ring;
    uint64_t    iters;
    int         n_prod, n_cons;
    size_t      msgsz;
    char       *pool;
} rte_mc_t;

static void *rte_prod_th(void *a) {
    rte_mc_t *c = a;
    char *pay = malloc(c->msgsz > 0 ? c->msgsz : 8);
    assert(pay != NULL);
    memset(pay, 0x12, c->msgsz > 0 ? c->msgsz : 8);

    for (uint64_t i = 0; i < c->iters; i++) {
        uintptr_t v;
        if (c->msgsz > 0 && c->pool) {
            char *slot = c->pool + ((i % RING_CAPACITY) * c->msgsz);
            memcpy(slot, pay, c->msgsz);
            v = (uintptr_t)slot;
        } else {
            v = 0x1234ULL;
        }
        rte_enqueue_wait(c->ring, v);
    }
    free(pay);
    return NULL;
}

static void *rte_cons_th(void *a) {
    rte_mc_t *c = a;
    uint64_t tgt = (uint64_t)c->n_prod * c->iters / (uint64_t)c->n_cons;
    uint64_t n = 0; uintptr_t v;
    uint32_t spins = 0;
    char sink[64];

    while (n < tgt) {
        if (rte_mc_dequeue(c->ring, &v) == 0) {
            if (c->msgsz > 0 && v) {
                memcpy(sink, (const void *)v, c->msgsz > sizeof(sink) ? sizeof(sink) : c->msgsz);
            }
            n++;
            spins = 0;
        } else {
            bench_spin_pause(&spins);
        }
    }
    return NULL;
}

double bench_rte_mpmc(int np, int nc, uint64_t iters_p, size_t msgsz) {
    size_t pool_sz = (msgsz > 0) ? (RING_CAPACITY * msgsz) : 0;
    char *pool = NULL;
    if (pool_sz > 0) {
        pool = mmap(NULL, pool_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        assert(pool != MAP_FAILED);
    }

    rte_mc_t c = {
        .ring = rte_ring_alloc(RING_CAPACITY),
        .iters = iters_p,
        .n_prod = np,
        .n_cons = nc,
        .msgsz = msgsz,
        .pool = pool
    };

    pthread_t *pt = malloc((size_t)np * sizeof(pthread_t));
    pthread_t *ct = malloc((size_t)nc * sizeof(pthread_t));
    assert(pt != NULL && ct != NULL);

    uint64_t t0 = comp_get_ns();
    for (int i = 0; i < nc; i++) pthread_create(&ct[i], NULL, rte_cons_th, &c);
    for (int i = 0; i < np; i++) pthread_create(&pt[i], NULL, rte_prod_th, &c);
    for (int i = 0; i < np; i++) pthread_join(pt[i], NULL);
    for (int i = 0; i < nc; i++) pthread_join(ct[i], NULL);
    uint64_t t1 = comp_get_ns();

    double r = (double)((uint64_t)np * iters_p) / ((double)(t1 - t0) / 1e9);
    rte_ring_free(c.ring);
    if (pool) munmap(pool, pool_sz);
    free(pt); free(ct);
    return r;
}

/* --- LMAX Disruptor MPMC --- */
typedef struct {
    disruptor_ring_t *ring;
    uint64_t          iters;
    int64_t           total_msgs;
    size_t            msgsz;
    char             *pool;
} disr_mc_t;

static void *disr_prod_th(void *a) {
    disr_mc_t *c = a;
    char *pay = malloc(c->msgsz > 0 ? c->msgsz : 8);
    assert(pay != NULL);
    memset(pay, 0x56, c->msgsz > 0 ? c->msgsz : 8);

    for (uint64_t i = 0; i < c->iters; i++) {
        uintptr_t v;
        if (c->msgsz > 0 && c->pool) {
            char *slot = c->pool + ((i % RING_CAPACITY) * c->msgsz);
            memcpy(slot, pay, c->msgsz);
            v = (uintptr_t)slot;
        } else {
            v = 0x5678ULL;
        }
        disruptor_publish_mp(c->ring, v);
    }
    free(pay);
    return NULL;
}

static void *disr_cons_th(void *a) {
    disr_mc_t *c = a;
    uintptr_t v;
    char sink[64];
    while (disruptor_consume_wp(c->ring, c->total_msgs, &v)) {
        if (c->msgsz > 0 && v) {
            memcpy(sink, (const void *)v, c->msgsz > sizeof(sink) ? sizeof(sink) : c->msgsz);
        }
    }
    return NULL;
}

double bench_disruptor_mpmc(int np, int nc, uint64_t iters_p, size_t msgsz) {
    size_t pool_sz = (msgsz > 0) ? (RING_CAPACITY * msgsz) : 0;
    char *pool = NULL;
    if (pool_sz > 0) {
        pool = mmap(NULL, pool_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        assert(pool != MAP_FAILED);
    }

    disr_mc_t c = {
        .ring = disruptor_alloc(RING_CAPACITY),
        .iters = iters_p,
        .total_msgs = (int64_t)((uint64_t)np * iters_p),
        .msgsz = msgsz,
        .pool = pool
    };

    pthread_t *pt = malloc((size_t)np * sizeof(pthread_t));
    pthread_t *ct = malloc((size_t)nc * sizeof(pthread_t));
    assert(pt != NULL && ct != NULL);

    uint64_t t0 = comp_get_ns();
    for (int i = 0; i < nc; i++) pthread_create(&ct[i], NULL, disr_cons_th, &c);
    for (int i = 0; i < np; i++) pthread_create(&pt[i], NULL, disr_prod_th, &c);
    for (int i = 0; i < np; i++) pthread_join(pt[i], NULL);
    for (int i = 0; i < nc; i++) pthread_join(ct[i], NULL);
    uint64_t t1 = comp_get_ns();

    double r = (double)((uint64_t)np * iters_p) / ((double)(t1 - t0) / 1e9);
    disruptor_free(c.ring);
    if (pool) munmap(pool, pool_sz);
    free(pt); free(ct);
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
    char *pay = malloc(c->msgsz);
    assert(pay != NULL);
    memset(pay, 0xAB, c->msgsz);

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

double bench_rbi_mpmc(int np, int nc, uint64_t iters_p, rbipc_ring_t *ring, size_t msgsz) {
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
    assert(pt != NULL && ct != NULL);

    uint64_t t0 = comp_get_ns();
    for (int i = 0; i < nc; i++) pthread_create(&ct[i], NULL, rbi_cons_th, &c);
    for (int i = 0; i < np; i++) pthread_create(&pt[i], NULL, rbi_prod_th, &c);
    for (int i = 0; i < np; i++) pthread_join(pt[i], NULL);
    for (int i = 0; i < nc; i++) pthread_join(ct[i], NULL);
    uint64_t t1 = comp_get_ns();

    double r = (double)((uint64_t)np * iters_p) / ((double)(t1 - t0) / 1e9);
    free(pt); free(ct);
    return r;
}

void run_bench_mpmc(FILE *json_fp, uint64_t mpmc_iters) {
    char hdr_buf[128];
    snprintf(hdr_buf, sizeof(hdr_buf), "BENCH 5: MPMC Thread Scalability -- 64B Payloads, %lu Iterations / Producer", (unsigned long)mpmc_iters);
    comp_print_hdr(hdr_buf);

    typedef struct { int np, nc; } cfg_t;
    cfg_t cfgs[] = { {1,1}, {2,2}, {4,2}, {4,4} };
    int ncfgs = (int)(sizeof(cfgs)/sizeof(cfgs[0]));

    printf("\n  %s%-10s  %20s  %20s  %18s  %18s%s\n",
           COL_BOLD, "Config", "librbipc ZeroTrust", "librbipc Trusted", "rte_ring (Mops/s)", "Disruptor (Mops/s)", COL_RESET);
    printf("  %-10s  %20s  %20s  %18s  %18s\n",
           "----------", "--------------------", "--------------------", "------------------", "------------------");

    if (json_fp) fprintf(json_fp, "  \"mpmc_scalability\": [\n");

    for (int i = 0; i < ncfgs; i++) {
        /* A. librbipc Zero-Trust Standard */
        rbipc_ring_t *r_zt = NULL;
        rbipc_destroy("/rbench_mpmc_zt");
        assert(rbipc_create_ex("/rbench_mpmc_zt", RING_CAPACITY, 128, RBIPC_SHM_NAMED, 0660, &r_zt) == RBIPC_OK);
        rbipc_set_wait_policy(r_zt, RBIPC_WAIT_ZERO_SPIN);
        double rbi_zt = bench_rbi_mpmc(cfgs[i].np, cfgs[i].nc, mpmc_iters, r_zt, 64);
        rbipc_detach(r_zt);
        rbipc_destroy("/rbench_mpmc_zt");

        /* B. librbipc Full-Capability Trusted & Packed */
        rbipc_ring_t *r_tr = NULL;
        rbipc_destroy("/rbench_mpmc_tr");
        assert(rbipc_create_ex("/rbench_mpmc_tr", RING_CAPACITY, 128,
                               RBIPC_SHM_NAMED | RBIPC_FLAG_TRUSTED | RBIPC_FLAG_PACKED, 0660, &r_tr) == RBIPC_OK);
        rbipc_set_wait_policy(r_tr, RBIPC_WAIT_ZERO_SPIN);
        double rbi_tr = bench_rbi_mpmc(cfgs[i].np, cfgs[i].nc, mpmc_iters, r_tr, 64);
        rbipc_detach(r_tr);
        rbipc_destroy("/rbench_mpmc_tr");

        /* C. DPDK rte_ring with 64B copy */
        double rte = bench_rte_mpmc(cfgs[i].np, cfgs[i].nc, mpmc_iters, 64);

        /* D. LMAX Disruptor with 64B copy */
        double dis = bench_disruptor_mpmc(cfgs[i].np, cfgs[i].nc, mpmc_iters, 64);

        printf("  %dP-%-6dC  %20.2f  %20.2f  %18.2f  %18.2f\n",
               cfgs[i].np, cfgs[i].nc,
               rbi_zt / 1e6, rbi_tr / 1e6, rte / 1e6, dis / 1e6);

        if (json_fp) {
            fprintf(json_fp, "    {\"config\": \"%dP-%dC\", \"librbipc_zero_trust\": %.2f, \"librbipc\": %.2f, \"rte_ring\": %.2f, \"disruptor\": %.2f}%s\n",
                    cfgs[i].np, cfgs[i].nc, rbi_zt, rbi_tr, rte, dis, (i == ncfgs - 1 ? "" : ","));
        }
    }

    if (json_fp) fprintf(json_fp, "  ],\n");
}
