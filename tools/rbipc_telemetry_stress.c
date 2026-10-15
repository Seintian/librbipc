/**
 * @file rbipc_telemetry_stress.c
 * @brief High-contention multi-threaded workload generator for stress testing and telemetry demonstration.
 *
 * @details Spawns concurrent producers and consumers with dynamic backlog oscillation,
 * controlled write aborts (poisoned slots), simulated consumer stall leases, and periodic audits
 * to showcase comprehensive shared memory telemetry under realistic production-grade stress.
 *
 * @author Seintian
 * @date 2026
 * @copyright Apache License 2.0
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "rbipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <errno.h>

#define DEFAULT_RING_NAME "/rbipc_telemetry_demo"
#define DEFAULT_CAPACITY  256U
#define DEFAULT_SLOT_SIZE 128U
#define NUM_PRODUCERS     4
#define NUM_CONSUMERS     4
#define LEASE_TIMEOUT_NS  (50 * 1000 * 1000ULL) /* 50 ms */

static volatile sig_atomic_t g_stop = 0;
static _Atomic uint64_t g_msgs_produced = 0;
static _Atomic uint64_t g_msgs_consumed = 0;

static void handle_sig(int sig) {
    (void)sig;
    g_stop = 1;
}

typedef struct {
    rbipc_ring_t *ring;
    int thread_id;
} worker_args_t;

static void *producer_worker(void *arg) {
    worker_args_t *args = (worker_args_t *)arg;
    rbipc_ring_t *ring = args->ring;
    int tid = args->thread_id;
    uint32_t counter = 0;

    char payload[64];
    while (!g_stop) {
        snprintf(payload, sizeof(payload), "Worker-%d Msg-%u", tid, counter++);
        uint32_t len = (uint32_t)strlen(payload) + 1;

        void *wbuf = NULL;
        uint64_t ticket = 0;
        int rc = rbipc_reserve_write(ring, len, &wbuf, &ticket);
        if (rc == RBIPC_ERR_SHUTDOWN) break;
        if (rc != RBIPC_OK) {
            if (g_stop) break;
            continue;
        }

        /* Simulated 0.2% producer abort rate to populate poisoned_slots_count telemetry */
        if ((counter % 500) == 0) {
            rbipc_abort_write(ring, ticket);
            continue;
        }

        memcpy(wbuf, payload, len);
        rc = rbipc_commit_write(ring, ticket, len);
        if (rc == RBIPC_OK) {
            atomic_fetch_add_explicit(&g_msgs_produced, 1, memory_order_relaxed);
        }
    }
    return NULL;
}

static void *consumer_worker(void *arg) {
    worker_args_t *args = (worker_args_t *)arg;
    rbipc_ring_t *ring = args->ring;
    int tid = args->thread_id;
    uint32_t consumed = 0;

    while (!g_stop) {
        const void *rbuf = NULL;
        uint32_t rlen = 0;
        uint64_t ticket = 0;
        int rc = rbipc_read_acquire_timeout(ring, 10 * 1000 * 1000ULL, &rbuf, &rlen, &ticket);
        if (rc == RBIPC_ERR_SHUTDOWN || (g_stop && rc == RBIPC_ERR_TIMEOUT)) break;
        if (rc == RBIPC_ERR_TIMEOUT) continue;

        if (rc == RBIPC_ERR_POISONED) {
            /* Safely bypass and recycle poisoned slot */
            rbipc_read_release(ring, ticket);
            continue;
        }

        if (rc == RBIPC_OK) {
            consumed++;
            /* Occasionally simulate consumer stall on worker 0 exceeding lease duration */
            if (tid == 0 && (consumed % 400) == 0) {
                struct timespec ts = { .tv_sec = 0, .tv_nsec = 70 * 1000 * 1000L }; /* 70 ms > 50 ms lease */
                nanosleep(&ts, NULL);
            }

            /* Release slot; if lease expired, rbipc_read_release returns RBIPC_ERR_TIMEOUT */
            rbipc_read_release(ring, ticket);
            atomic_fetch_add_explicit(&g_msgs_consumed, 1, memory_order_relaxed);
        }
    }
    return NULL;
}

static void *auditor_worker(void *arg) {
    rbipc_ring_t *ring = (rbipc_ring_t *)arg;
    while (!g_stop) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 25 * 1000 * 1000L }; /* Audit every 25 ms */
        nanosleep(&ts, NULL);
        if (g_stop) break;
        int revoked = rbipc_revoke_expired_leases(ring);
        (void)revoked;
    }
    return NULL;
}

int main(int argc, char **argv) {
    const char *shm_name = (argc > 1 && argv[1][0] != '-') ? argv[1] : DEFAULT_RING_NAME;
    double duration_sec = 3.0;

    if (argc > 2) {
        char *endptr = NULL;
        errno = 0;
        double val = strtod(argv[2], &endptr);
        if (errno != 0 || endptr == argv[2] || *endptr != '\0' || val <= 0.0) {
            fprintf(stderr, "Error: Invalid duration '%s'. Expected positive number.\n", argv[2]);
            return 1;
        }
        duration_sec = val;
    }

    printf("[rbipc_telemetry_stress] Initializing ring '%s' (Capacity: %u, Lease: %lu ms)...\n",
           shm_name, DEFAULT_CAPACITY, (unsigned long)(LEASE_TIMEOUT_NS / 1000000ULL));

    rbipc_destroy(shm_name);

    rbipc_ring_t *creator_ring = NULL;
    int rc = rbipc_create_ex(shm_name, DEFAULT_CAPACITY, DEFAULT_SLOT_SIZE,
                             RBIPC_ROLE_PRODUCER | RBIPC_ROLE_CONSUMER, 0660, &creator_ring);
    if (rc != RBIPC_OK) {
        fprintf(stderr, "Error creating ring '%s': %s (code %d)\n", shm_name, rbipc_strerror(rc), rc);
        return 1;
    }

    rc = rbipc_set_lease_timeout(creator_ring, LEASE_TIMEOUT_NS);
    if (rc != RBIPC_OK) {
        fprintf(stderr, "Error setting lease timeout: %s\n", rbipc_strerror(rc));
        rbipc_destroy(shm_name);
        return 1;
    }

    struct sigaction sa = {};
    sa.sa_handler = handle_sig;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    pthread_t prod_threads[NUM_PRODUCERS];
    pthread_t cons_threads[NUM_CONSUMERS];
    pthread_t audit_thread;
    worker_args_t p_args[NUM_PRODUCERS];
    worker_args_t c_args[NUM_CONSUMERS];
    rbipc_ring_t *p_rings[NUM_PRODUCERS];
    rbipc_ring_t *c_rings[NUM_CONSUMERS];

    /* Attach producers */
    for (int i = 0; i < NUM_PRODUCERS; ++i) {
        rc = rbipc_attach_ex(shm_name, RBIPC_ROLE_PRODUCER, &p_rings[i]);
        if (rc != RBIPC_OK) {
            fprintf(stderr, "Error attaching producer %d\n", i);
            return 1;
        }
        p_args[i].ring = p_rings[i];
        p_args[i].thread_id = i;
        pthread_create(&prod_threads[i], NULL, producer_worker, &p_args[i]);
    }

    /* Attach consumers */
    for (int i = 0; i < NUM_CONSUMERS; ++i) {
        rc = rbipc_attach_ex(shm_name, RBIPC_ROLE_CONSUMER, &c_rings[i]);
        if (rc != RBIPC_OK) {
            fprintf(stderr, "Error attaching consumer %d\n", i);
            return 1;
        }
        c_args[i].ring = c_rings[i];
        c_args[i].thread_id = i;
        pthread_create(&cons_threads[i], NULL, consumer_worker, &c_args[i]);
    }

    pthread_create(&audit_thread, NULL, auditor_worker, creator_ring);

    printf("[rbipc_telemetry_stress] Running %d Producers and %d Consumers for %.1f seconds...\n",
           NUM_PRODUCERS, NUM_CONSUMERS, duration_sec);

    if (duration_sec > 0.0) {
        struct timespec ts = {
            .tv_sec = (time_t)duration_sec,
            .tv_nsec = (long)((duration_sec - (double)(time_t)duration_sec) * 1e9)
        };
        nanosleep(&ts, NULL);
    } else {
        while (!g_stop) {
            sleep(1);
        }
    }

    g_stop = 1;
    rbipc_signal_shutdown(creator_ring);

    /* Join threads */
    for (int i = 0; i < NUM_PRODUCERS; ++i) {
        pthread_join(prod_threads[i], NULL);
        rbipc_detach(p_rings[i]);
    }
    for (int i = 0; i < NUM_CONSUMERS; ++i) {
        pthread_join(cons_threads[i], NULL);
        rbipc_detach(c_rings[i]);
    }
    pthread_join(audit_thread, NULL);

    uint64_t total_p = atomic_load_explicit(&g_msgs_produced, memory_order_relaxed);
    uint64_t total_c = atomic_load_explicit(&g_msgs_consumed, memory_order_relaxed);
    printf("[rbipc_telemetry_stress] Execution complete: %lu messages produced, %lu messages consumed.\n",
           (unsigned long)total_p, (unsigned long)total_c);

    rbipc_detach(creator_ring);
    rbipc_destroy(shm_name);
    return 0;
}
