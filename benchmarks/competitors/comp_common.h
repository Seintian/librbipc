/**
 * @file comp_common.h
 * @brief Common definitions, data structures, and utilities for competitors benchmark.
 */

#ifndef RBIPC_COMP_COMMON_H
#define RBIPC_COMP_COMMON_H

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
#include "../bench_common.h"

#define NSEC_PER_SEC        1000000000ULL
#define BENCH_ITERATIONS    200000ULL
#define WARMUP_ITERATIONS   2000ULL
#define LAT_ITERATIONS      30000ULL
#define RING_CAPACITY       4096U
#include "rbipc.h"
#define CACHE_LINE_SIZE     RBIPC_CACHE_LINE

#define COL_RESET   "\033[0m"
#define COL_BOLD    "\033[1m"
#define COL_RED     "\033[31m"
#define COL_GREEN   "\033[32m"
#define COL_YELLOW  "\033[33m"
#define COL_CYAN    "\033[36m"
#define COL_MAGENTA "\033[35m"

#define MQ_NAME_AB  "/rbench_mq_ab"
#define MQ_NAME_BA  "/rbench_mq_ba"
#define MQ_PRIO     1

typedef enum {
    TRANSFER_VALUE_COPY,
    TRANSFER_REFERENCE_PASSING
} transfer_semantic_t;

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

static inline uint64_t comp_get_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * NSEC_PER_SEC + (uint64_t)ts.tv_nsec;
}

static inline int comp_cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static inline uint64_t comp_pct64(uint64_t *arr, size_t n, double p) {
    if (!n) return 0;
    size_t i = (size_t)(p / 100.0 * (double)n);
    if (i >= n) i = n - 1;
    return arr[i];
}

static inline void comp_print_sep(void) {
    printf("%s%s----------------------------------------------------------------------------------------%s\n",
           COL_BOLD, COL_CYAN, COL_RESET);
}

static inline void comp_print_hdr(const char *t) {
    comp_print_sep();
    printf("%s%s  %s%s\n", COL_BOLD, COL_YELLOW, t, COL_RESET);
    comp_print_sep();
}

static inline void comp_print_result(const result_t *r) {
    printf("  %-25s | %10.2f Mops/s | %9.2f MB/s | Min: %4lu ns | p50: %5lu ns | p99: %6lu ns | p99.9: %7lu ns\n",
           r->name, r->tput_ops / 1e6, r->tput_mbs,
           (unsigned long)r->lat_min, (unsigned long)r->lat_p50,
           (unsigned long)r->lat_p99, (unsigned long)r->lat_p999);
}

static inline void comp_print_table(result_t *arr, int n, const char *kind) {
    comp_print_sep();
    printf("  %s%s Benchmark Summary (%s)%s\n", COL_BOLD, kind, (n > 0 ? arr[0].name : ""), COL_RESET);
    comp_print_sep();
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
    comp_print_sep();
}

static inline double comp_calc_idle_cpu(struct rusage *r0, struct rusage *r1, uint64_t elapsed_ns) {
    uint64_t cpu_us =
        (uint64_t)(r1->ru_utime.tv_sec  - r0->ru_utime.tv_sec)  * 1000000u +
        (uint64_t)(r1->ru_utime.tv_usec - r0->ru_utime.tv_usec) +
        (uint64_t)(r1->ru_stime.tv_sec  - r0->ru_stime.tv_sec)  * 1000000u +
        (uint64_t)(r1->ru_stime.tv_usec - r0->ru_stime.tv_usec);
    return (double)cpu_us / ((double)elapsed_ns / 1000.0) * 100.0;
}

/* DPDK rte_ring interface */
typedef struct rte_ring rte_ring_t;
rte_ring_t *rte_ring_alloc(uint32_t cap);
void rte_ring_free(rte_ring_t *r);
int rte_mp_enqueue(rte_ring_t *r, uintptr_t val);
int rte_mc_dequeue(rte_ring_t *r, uintptr_t *out);
void rte_enqueue_wait(rte_ring_t *r, uintptr_t v);
void rte_dequeue_wait(rte_ring_t *r, uintptr_t *v);
uint32_t rte_mp_enqueue_burst(rte_ring_t *r, const uintptr_t *vals, uint32_t n);
uint32_t rte_mc_dequeue_burst(rte_ring_t *r, uintptr_t *out, uint32_t n);

/* Disruptor interface */
typedef struct disruptor_ring disruptor_ring_t;
disruptor_ring_t *disruptor_alloc(uint32_t cap);
void disruptor_free(disruptor_ring_t *d);
void disruptor_publish_sp(disruptor_ring_t *d, int64_t seq, uintptr_t val);
void disruptor_publish_mp(disruptor_ring_t *d, uintptr_t val);
bool disruptor_consume_wp(disruptor_ring_t *d, int64_t max_seq, uintptr_t *out);
uint32_t disruptor_publish_batch(disruptor_ring_t *d, const uintptr_t *vals, uint32_t n);
uint32_t disruptor_consume_batch(disruptor_ring_t *d, int64_t max_seq, uintptr_t *out, uint32_t max_n);

/* POSIX mqueue interface */
mqd_t mq_make(const char *name, size_t msgsz, long maxmsg);

/* Benchmark runners */
result_t bench_rbipc_tput(size_t msgsz, uint64_t iters, transfer_semantic_t mode, uint32_t flags, const char *label);
result_t bench_rte_tput(size_t msgsz, uint64_t iters, transfer_semantic_t mode);
result_t bench_disruptor_tput(size_t msgsz, uint64_t iters, transfer_semantic_t mode);
result_t bench_mq_tput(size_t msgsz, uint64_t iters);
result_t bench_uds_tput(size_t msgsz, uint64_t iters);

result_t bench_rbipc_lat(uint64_t iters, uint32_t flags, const char *label);
result_t bench_rte_lat(uint64_t iters);
result_t bench_disruptor_lat(uint64_t iters);
result_t bench_mq_lat(uint64_t iters);
result_t bench_uds_lat(uint64_t iters);

double bench_rbi_mpmc(int np, int nc, uint64_t iters_p, rbipc_ring_t *ring, size_t msgsz);
double bench_rte_mpmc(int np, int nc, uint64_t iters_p, size_t msgsz);
double bench_disruptor_mpmc(int np, int nc, uint64_t iters_p, size_t msgsz);

void run_bench_idle_cpu(FILE *json_fp);
void run_bench_mpmc(FILE *json_fp, uint64_t mpmc_iters);
void run_bench_batch_scaling(FILE *json_fp, uint64_t batch_iters);

#endif /* RBIPC_COMP_COMMON_H */
