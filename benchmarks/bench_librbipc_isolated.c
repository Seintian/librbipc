/**
 * @file bench_librbipc_isolated.c
 * @brief Standalone benchmarking harness for librbipc to enable high-resolution Linux perf profiling.
 *
 * Evaluates:
 * 1. Pointer-only / 8-byte value exchange (exact apples-to-apples comparison with DPDK and Disruptor)
 * 2. Full 64-byte payload memcpy exchange
 * 3. B-Queue vector batching (batch size 32)
 *
 * Supports both intra-process multi-threading and multi-process fork models.
 */

#include "rbipc.h"
#include "bench_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/wait.h>
#include <time.h>
#include <assert.h>

#define BENCH_SHM_NAME "/rbench_isolated"
#define RING_CAPACITY  2048
#define NSEC_PER_SEC   1000000000ULL

static inline uint64_t get_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * NSEC_PER_SEC + (uint64_t)ts.tv_nsec;
}

typedef enum {
    BENCH_MODE_PTR = 0,    /* 8-byte pointer/scalar exchange */
    BENCH_MODE_COPY = 1,   /* 64-byte full payload memcpy */
    BENCH_MODE_BATCH = 2   /* B-Queue vector batching (32 items) */
} bench_mode_t;

typedef struct {
    rbipc_ring_t *ring;
    uint64_t      iters;
    bench_mode_t  mode;
    uint32_t      batch_size;
} bench_ctx_t;

/* --- Consumer Worker --- */
static void *consumer_fn(void *arg) {
    bench_ctx_t *ctx = (bench_ctx_t *)arg;
    rbipc_ring_t *r = ctx->ring;
    uint64_t iters = ctx->iters;

    if (ctx->mode == BENCH_MODE_BATCH) {
        uint32_t bsize = ctx->batch_size;
        uint64_t count = 0;
        rbipc_rovec_t rovs[64];
        uint64_t tickets[64];

        while (count < iters) {
            uint32_t req = (uint32_t)(iters - count);
            if (req > bsize) req = bsize;
            uint32_t acquired = 0;
            while (rbipc_read_acquire_batch(r, req, rovs, &acquired) != RBIPC_OK) {
                __asm__ volatile("pause" ::: "memory");
            }
            for (uint32_t k = 0; k < acquired; k++) {
                tickets[k] = rovs[k].ticket;
            }
            rbipc_read_release_batch(r, acquired, tickets);
            count += acquired;
        }
    } else {
        const void *buf;
        uint32_t len;
        uint64_t ticket;

        for (uint64_t i = 0; i < iters; i++) {
            while (rbipc_read_acquire(r, &buf, &len, &ticket) != RBIPC_OK) {
                __asm__ volatile("pause" ::: "memory");
            }
            if (ctx->mode == BENCH_MODE_PTR) {
                uintptr_t val = *(const uintptr_t *)buf;
                (void)val;
            }
            rbipc_read_release(r, ticket);
        }
    }
    return NULL;
}

/* --- Producer Worker --- */
static void *producer_fn(void *arg) {
    bench_ctx_t *ctx = (bench_ctx_t *)arg;
    rbipc_ring_t *r = ctx->ring;
    uint64_t iters = ctx->iters;

    if (ctx->mode == BENCH_MODE_BATCH) {
        uint32_t bsize = ctx->batch_size;
        uint64_t sent = 0;
        rbipc_iovec_t iovs[64];
        uint64_t tickets[64];
        uint32_t lens[64];

        while (sent < iters) {
            uint32_t req = (uint32_t)(iters - sent);
            if (req > bsize) req = bsize;
            uint32_t reserved = 0;
            while (rbipc_reserve_write_batch(r, req, iovs, &reserved) != RBIPC_OK) {
                __asm__ volatile("pause" ::: "memory");
            }
            for (uint32_t k = 0; k < reserved; k++) {
                tickets[k] = iovs[k].ticket;
                lens[k] = 64;
                *(uintptr_t *)iovs[k].buf = (uintptr_t)(sent + k);
            }
            rbipc_commit_write_batch(r, reserved, tickets, lens);
            sent += reserved;
        }
    } else if (ctx->mode == BENCH_MODE_PTR) {
        void *buf;
        uint64_t ticket;

        for (uint64_t i = 0; i < iters; i++) {
            while (rbipc_reserve_write(r, 8, &buf, &ticket) != RBIPC_OK) {
                __asm__ volatile("pause" ::: "memory");
            }
            *(uintptr_t *)buf = (uintptr_t)i;
            rbipc_commit_write(r, ticket, 8);
        }
    } else {
        char payload[64];
        memset(payload, 0x5A, sizeof(payload));
        void *buf;
        uint64_t ticket;

        for (uint64_t i = 0; i < iters; i++) {
            while (rbipc_reserve_write(r, 64, &buf, &ticket) != RBIPC_OK) {
                __asm__ volatile("pause" ::: "memory");
            }
            memcpy(buf, payload, 64);
            rbipc_commit_write(r, ticket, 64);
        }
    }
    return NULL;
}

int main(int argc, char **argv) {
    bench_mode_t mode = BENCH_MODE_PTR;
    uint64_t iters = 1000000ULL;
    uint32_t policy = RBIPC_WAIT_ADAPTIVE;
    bool use_fork = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--ptr") == 0) {
            mode = BENCH_MODE_PTR;
        } else if (strcmp(argv[i], "--copy") == 0) {
            mode = BENCH_MODE_COPY;
        } else if (strcmp(argv[i], "--batch") == 0) {
            mode = BENCH_MODE_BATCH;
        } else if (strcmp(argv[i], "--zero-spin") == 0) {
            policy = RBIPC_WAIT_ZERO_SPIN;
        } else if (strcmp(argv[i], "--adaptive") == 0) {
            policy = RBIPC_WAIT_ADAPTIVE;
        } else if (strcmp(argv[i], "--fork") == 0) {
            use_fork = true;
        } else if (strcmp(argv[i], "--iters") == 0 && i + 1 < argc) {
            iters = strtoull(argv[++i], NULL, 10);
        }
    }

    const char *mode_str = (mode == BENCH_MODE_PTR) ? "8-Byte Pointer / Scalar Exchange"
                         : (mode == BENCH_MODE_COPY) ? "64-Byte Payload memcpy"
                                                     : "B-Queue Vector Batch (32 items)";
    const char *pol_str  = (policy == RBIPC_WAIT_ADAPTIVE) ? "ADAPTIVE" : "ZERO_SPIN";

    printf("======================================================================\n");
    printf("   librbipc Isolated Profiling Harness\n");
    printf("   Mode       : %s\n", mode_str);
    printf("   Policy     : %s\n", pol_str);
    printf("   Iterations : %lu\n", (unsigned long)iters);
    printf("   Model      : %s\n", use_fork ? "Multi-Process (fork)" : "Multi-Thread (pthread)");
    printf("======================================================================\n");

    rbipc_destroy(BENCH_SHM_NAME);
    rbipc_ring_t *rp = NULL, *rc = NULL;
    int rc1 = rbipc_create(BENCH_SHM_NAME, RING_CAPACITY, 128, &rp);
    int rc2 = rbipc_attach(BENCH_SHM_NAME, &rc);
    assert(rc1 == RBIPC_OK && rc2 == RBIPC_OK);

    rbipc_set_wait_policy(rp, policy);
    rbipc_set_wait_policy(rc, policy);

    bench_ctx_t prod_ctx = { .ring = rp, .iters = iters, .mode = mode, .batch_size = 32 };
    bench_ctx_t cons_ctx = { .ring = rc, .iters = iters, .mode = mode, .batch_size = 32 };

    uint64_t t0 = get_ns();

    if (use_fork) {
        int sync_pipe[2];
        assert(pipe(sync_pipe) == 0);
        pid_t pid = fork();
        if (pid == 0) {
            close(sync_pipe[0]);
            consumer_fn(&cons_ctx);
            write(sync_pipe[1], "OK", 2);
            close(sync_pipe[1]);
            rbipc_detach(rc);
            _exit(0);
        } else {
            close(sync_pipe[1]);
            producer_fn(&prod_ctx);
            char ack[4];
            (void)read(sync_pipe[0], ack, 2);
            close(sync_pipe[0]);
            waitpid(pid, NULL, 0);
        }
    } else {
        pthread_t cons_th;
        pthread_create(&cons_th, NULL, consumer_fn, &cons_ctx);
        producer_fn(&prod_ctx);
        pthread_join(cons_th, NULL);
    }

    uint64_t t1 = get_ns();
    uint64_t elapsed_ns = t1 - t0;
    double ops = (double)iters / ((double)elapsed_ns / 1e9);
    double ns_per_op = (double)elapsed_ns / (double)iters;

    printf("\n--> Result:\n");
    printf("    Elapsed Time : %.4f seconds (%.2f ns/op)\n", (double)elapsed_ns / 1e9, ns_per_op);
    printf("    Throughput   : %.2f Mops/s (%.2f ops/sec)\n", ops / 1e6, ops);
    printf("======================================================================\n");
    /* Export JSON Telemetry */
    bench_hardware_info_t hw_info;
    bench_get_hardware_info(&hw_info);
    char json_path[256];
    bench_get_result_path(json_path, sizeof(json_path), "isolated_benchmark.json");
    FILE *fp = fopen(json_path, "w");
    if (fp) {
        fprintf(fp, "{\n");
        fprintf(fp, "  \"metadata\": {\n");
        fprintf(fp, "    \"cpu_model\": \"%s\",\n", hw_info.cpu_model);
        fprintf(fp, "    \"kernel\": \"%s\",\n", hw_info.kernel);
        fprintf(fp, "    \"profile\": \"%s\",\n", hw_info.profile_name);
        fprintf(fp, "    \"mode\": \"%s\",\n", mode_str);
        fprintf(fp, "    \"policy\": \"%s\",\n", pol_str);
        fprintf(fp, "    \"model\": \"%s\",\n", use_fork ? "Multi-Process (fork)" : "Multi-Thread (pthread)");
        fprintf(fp, "    \"iterations\": %lu\n", (unsigned long)iters);
        fprintf(fp, "  },\n");
        fprintf(fp, "  \"result\": {\n");
        fprintf(fp, "    \"elapsed_seconds\": %.6f,\n", (double)elapsed_ns / 1e9);
        fprintf(fp, "    \"latency_ns_per_op\": %.2f,\n", ns_per_op);
        fprintf(fp, "    \"throughput_mops\": %.2f,\n", ops / 1e6);
        fprintf(fp, "    \"throughput_ops_sec\": %.2f\n", ops);
        fprintf(fp, "  }\n");
        fprintf(fp, "}\n");
        fclose(fp);
        printf("[INFO] JSON output saved to %s\n", json_path);
    }

    rbipc_detach(rc);
    rbipc_detach(rp);
    rbipc_destroy(BENCH_SHM_NAME);
    return 0;
}
