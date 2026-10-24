/**
 * @file idle_common.h
 * @brief Common definitions, data structures, and utilities for idle strategies benchmark.
 */

#ifndef RBIPC_IDLE_COMMON_H
#define RBIPC_IDLE_COMMON_H

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
#include <pthread.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <sched.h>

#include "rbipc.h"
#include "../bench_common.h"

#define NSEC_PER_SEC          1000000000ULL
#define RING_CAPACITY         4096U
#include "rbipc.h"
#define CACHE_LINE_SIZE       RBIPC_CACHE_LINE
#define MSG_PAYLOAD_SIZE      64U

#define COL_RESET   "\033[0m"
#define COL_BOLD    "\033[1m"
#define COL_RED     "\033[31m"
#define COL_GREEN   "\033[32m"
#define COL_YELLOW  "\033[33m"
#define COL_CYAN    "\033[36m"
#define COL_MAGENTA "\033[35m"

typedef struct {
    char     strategy_name[48];
    char     category[24];
    double   tput_mops;
    double   tput_mbs;
    uint64_t lat_min;
    uint64_t lat_p50;
    uint64_t lat_p90;
    uint64_t lat_p99;
    uint64_t lat_p999;
    uint64_t lat_max;
    uint64_t wake_min;
    uint64_t wake_p50;
    uint64_t wake_p90;
    uint64_t wake_p99;
    uint64_t wake_max;
    double   idle_cpu_pct;
    long     voluntary_ctx;
    long     involuntary_ctx;
} idle_bench_result_t;

typedef enum {
    LMAX_STRAT_BUSYSPIN,
    LMAX_STRAT_YIELDING,
    LMAX_STRAT_SLEEPING,
    LMAX_STRAT_BLOCKING,
    LMAX_STRAT_LITEBLOCKING,
    LMAX_STRAT_PHASEDBACKOFF
} lmax_strat_t;

typedef struct dpdk_ring dpdk_ring_t;
typedef struct lmax_ring lmax_ring_t;
typedef struct {
    rbipc_ring_t *ring_ab_prod;
    rbipc_ring_t *ring_ab_cons;
    rbipc_ring_t *ring_ba_prod;
    rbipc_ring_t *ring_ba_cons;
} rbipc_env_t;

extern uint64_t g_tput_iters;
extern uint64_t g_tput_warmup;
extern uint64_t g_lat_iters;
extern uint64_t g_wake_iters;
extern uint64_t g_idle_obs_ns;
extern uint64_t g_wake_gap_ns;

static inline uint64_t idle_get_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * NSEC_PER_SEC + (uint64_t)ts.tv_nsec;
}

static inline uint64_t idle_get_thread_cpu_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * NSEC_PER_SEC + (uint64_t)ts.tv_nsec;
}

static inline void idle_cpu_pause(void) {
    __asm__ volatile("pause" ::: "memory");
}

static inline int idle_cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static inline uint64_t idle_pct64(uint64_t *arr, size_t n, double p) {
    if (!n) return 0;
    size_t i = (size_t)(p / 100.0 * (double)n);
    if (i >= n) i = n - 1;
    return arr[i];
}

/* DPDK prototypes */
dpdk_ring_t *dpdk_ring_alloc(uint32_t cap);
void dpdk_ring_reset(dpdk_ring_t *r);
void dpdk_ring_free(dpdk_ring_t *r);
double run_dpdk_tput(dpdk_ring_t *r, int strat_mode);
void run_dpdk_lat(dpdk_ring_t *ab, dpdk_ring_t *ba, int strat, uint64_t *samples);
void run_dpdk_wake(dpdk_ring_t *r, int strat, uint64_t *wake_samples);
double measure_dpdk_idle_cpu(dpdk_ring_t *r, int strat, long *vol_ctx, long *invol_ctx);

/* Disruptor prototypes */
lmax_ring_t *lmax_ring_alloc(uint32_t cap);
void lmax_ring_reset(lmax_ring_t *d);
void lmax_ring_free(lmax_ring_t *d);
double run_lmax_tput(lmax_ring_t *d, lmax_strat_t strat);
void run_lmax_lat(lmax_ring_t *ab, lmax_ring_t *ba, lmax_strat_t strat, uint64_t *samples);
void run_lmax_wake(lmax_ring_t *d, lmax_strat_t strat, uint64_t *wake_samples);
double measure_lmax_idle_cpu(lmax_ring_t *d, lmax_strat_t strat, long *vol_ctx, long *invol_ctx);

/* rbipc prototypes */
rbipc_env_t rbipc_env_init(const char *name_ab, const char *name_ba);
void rbipc_env_cleanup(rbipc_env_t *env, const char *name_ab, const char *name_ba);
double run_rbipc_tput(rbipc_env_t *env);
void run_rbipc_lat(rbipc_env_t *env, uint64_t *samples);
void run_rbipc_wake(rbipc_env_t *env, uint64_t *wake_samples);
double measure_rbipc_idle_cpu(rbipc_env_t *env, long *vol_ctx, long *invol_ctx);

/* Reporting & JSON */
void idle_record_res(idle_bench_result_t *results, size_t *n_res,
                     const char *name, const char *cat, double tput,
                     uint64_t *lats, uint64_t *wakes, double idle_cpu, long vol, long invol);
void idle_print_table(const idle_bench_result_t *results, size_t n_res);
void idle_export_json(const char *json_path, const bench_hardware_info_t *hw,
                      const idle_bench_result_t *results, size_t n_res);

#endif /* RBIPC_IDLE_COMMON_H */
