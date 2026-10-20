/**
 * @file comp_mpmc.c
 * @brief MPMC thread scalability benchmark for competitors.
 */

#include "comp_common.h"

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

double bench_rte_mpmc(int np, int nc, uint64_t iters_p) {
    rte_mc_t c = { .ring = rte_ring_alloc(RING_CAPACITY), .iters = iters_p, .n_prod = np, .n_cons = nc };
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

double bench_disruptor_mpmc(int np, int nc, uint64_t iters_p) {
    disr_mc_t c = {
        .ring = disruptor_alloc(RING_CAPACITY),
        .iters = iters_p,
        .total_msgs = (int64_t)((uint64_t)np * iters_p)
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
    snprintf(hdr_buf, sizeof(hdr_buf), "BENCH 5: MPMC Thread Scalability -- %lu Iterations / Producer", (unsigned long)mpmc_iters);
    comp_print_hdr(hdr_buf);

    rbipc_ring_t *mpmc_r = NULL;
    rbipc_destroy("/rbench_mpmc");
    if (rbipc_create_ex("/rbench_mpmc", RING_CAPACITY, 128, RBIPC_SHM_NAMED | RBIPC_FLAG_TRUSTED, 0660, &mpmc_r) == RBIPC_OK) {
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
}
