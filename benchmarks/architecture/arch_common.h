/**
 * @file arch_common.h
 * @brief Common definitions, data structures, and utilities for the architectural evaluation benchmark suite.
 */

#ifndef RBIPC_ARCH_COMMON_H
#define RBIPC_ARCH_COMMON_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "rbipc.h"
#include "rbipc_ipc.h"
#include "rbipc_pubsub.h"
#include "rbipc_simd.h"
#include "rbipc_futex.h"
#include "rbipc_slot.h"
#include "../bench_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sched.h>
#include <pthread.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <assert.h>
#include <math.h>
#include <errno.h>

#ifndef SYS_futex_waitv
#if defined(__x86_64__) || defined(__aarch64__)
#define SYS_futex_waitv 449
#endif
#endif

#define COL_RESET   "\033[0m"
#define COL_BOLD    "\033[1m"
#define COL_RED     "\033[31m"
#define COL_GREEN   "\033[32m"
#define COL_YELLOW  "\033[33m"
#define COL_CYAN    "\033[36m"
#define COL_MAGENTA "\033[35m"

#define ARCH_SHM_LAT_A     "/rbipc_opt_lat_a"
#define ARCH_SHM_LAT_B     "/rbipc_opt_lat_b"
#define ARCH_SHM_STREAM    "/rbipc_opt_stream"
#define ARCH_SHM_BATCH     "/rbipc_opt_batch"
#define ARCH_SHM_LIFECYCLE "/rbipc_opt_lifecycle"
#define ARCH_SHM_PUBSUB    "/rbipc_opt_pubsub"

typedef struct {
    const char *mode_name;
    uint32_t flags;
    uint32_t min_ns;
    uint32_t p25_ns;
    uint32_t p50_ns;
    uint32_t p75_ns;
    uint32_t p90_ns;
    uint32_t p99_ns;
    uint32_t p99_9_ns;
    uint32_t max_ns;
    double   mean_ns;
    double   reduction_pct;
} m1_latency_result_t;

typedef struct {
    const char *mode_name;
    uint32_t flags;
    bool pinned;
    double msgs_per_sec;
    double mb_per_sec;
    double avg_latency_ns;
} m2_streaming_result_t;

typedef struct {
    uint32_t batch_size;
    double t1_msgs_sec;
    double t1_mb_sec;
    double t1_latency_ns;
    double t23_msgs_sec;
    double t23_mb_sec;
    double t23_latency_ns;
    double t1_speedup_vs_b1;
    double t23_speedup_vs_b1;
} m3_batch_result_t;

typedef struct {
    double anon_ops_sec;
    double anon_latency_us;
    double named_ops_sec;
    double named_latency_us;
    double speedup_factor;
} m4_lifecycle_result_t;

typedef struct {
    const char *mode_name;
    uint32_t fanout;
    double pub_msgs_sec;
    double effective_msgs_sec;
    double effective_mb_sec;
} m5_pubsub_result_t;

typedef struct {
    const char *engine_name;
    double std_mslots_sec;
    double std_speedup;
    double packed_mslots_sec;
    double packed_speedup;
} m6_simd_result_t;

typedef struct {
    double mfd_1_ops_sec;
    double mfd_1_latency_us;
    double mfd_2_ops_sec;
    double mfd_2_latency_us;
    double mfd_4_ops_sec;
    double mfd_4_latency_us;
    double mfd_4_seq_ops_sec;
    double mfd_4_seq_latency_us;
    double mfd_4_speedup;

    double futex_memo_ops_sec;
    double futex_memo_lat_ns;
    double futex_syscall_ops_sec;
    double futex_syscall_lat_ns;
    double futex_speedup;

    bool hugetlb_available;
} m7_primitives_result_t;

typedef struct {
    const char *label;
    uint32_t    capacity;
    uint32_t    slot_size;
    double      buffer_mb;
    double      thp_msgs_sec;
    double      thp_mb_sec;
    double      thp_lat_ns;
    unsigned long thp_pmd_mapped_kb;
    double      std_msgs_sec;
    double      std_mb_sec;
    double      std_lat_ns;
    double      speedup_factor;
    double      latency_reduction_pct;
} m8_dynamic_hugepages_result_t;

static inline uint64_t arch_get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * RBIPC_NSEC_PER_SEC) + (uint64_t)ts.tv_nsec;
}

static inline int arch_compare_uint32(const void *a, const void *b) {
    uint32_t val_a = *(const uint32_t *)a;
    uint32_t val_b = *(const uint32_t *)b;
    if (val_a < val_b) return -1;
    if (val_a > val_b) return 1;
    return 0;
}

static inline void arch_pin_to_core(int cpu_id) {
    if (cpu_id < 0) return;
    long nprocs = sysconf(_SC_NPROCESSORS_ONLN);
    if (nprocs <= 0) return;
    int target = cpu_id % (int)nprocs;
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(target, &cpuset);
    sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
}

static inline void arch_unpin_core(void) {
    long nprocs = sysconf(_SC_NPROCESSORS_ONLN);
    if (nprocs <= 0) return;
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    for (int i = 0; i < (int)nprocs; ++i) {
        CPU_SET(i, &cpuset);
    }
    sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
}

static inline void arch_print_banner(const char *title) {
    printf("\n%s%s========================================================================================%s\n",
           COL_BOLD, COL_CYAN, COL_RESET);
    printf("%s%s  %s%s\n", COL_BOLD, COL_YELLOW, title, COL_RESET);
    printf("%s%s========================================================================================%s\n",
           COL_BOLD, COL_CYAN, COL_RESET);
}

static inline void arch_print_subhdr(const char *title) {
    printf("%s%s--- %s ---%s\n", COL_BOLD, COL_GREEN, title, COL_RESET);
}

/* Module function prototypes */
void run_module1_ping_pong(uint64_t iterations, uint64_t warmup, m1_latency_result_t results[4]);
void run_module2_streaming(uint64_t iterations, m2_streaming_result_t results[8]);
void run_module3_batching(uint64_t iterations_per_batch, m3_batch_result_t results[6]);
void run_module4_lifecycle(uint64_t cycles, m4_lifecycle_result_t *out);
void run_module5_pubsub(uint64_t iterations, m5_pubsub_result_t results[16]);
void run_module6_simd(uint64_t scan_iterations, m6_simd_result_t results[4]);
void run_module7_primitives(uint64_t fd_iterations, uint64_t futex_iterations, m7_primitives_result_t *out);
void run_module8_dynamic_hugepages(uint64_t iterations, m8_dynamic_hugepages_result_t out_results[4]);
void export_json_telemetry(const char *json_path, const bench_hardware_info_t *hw,
                           const m1_latency_result_t m1[4], const m2_streaming_result_t m2[8],
                           const m3_batch_result_t m3[6], const m4_lifecycle_result_t *m4,
                           const m5_pubsub_result_t m5[16], const m6_simd_result_t m6[4],
                           const m7_primitives_result_t *m7, const m8_dynamic_hugepages_result_t m8[4]);

#endif /* RBIPC_ARCH_COMMON_H */
