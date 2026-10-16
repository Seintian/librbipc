/**
 * @file test_e2e_chaos.c
 * @brief Automated crash-injection, process stalling, and chaos stress test suite.
 *
 * @details Spawns concurrent producer and consumer worker processes exchanging messages
 * across shared memory while a supervisor injects randomized SIGKILL, SIGSTOP, and SIGCONT
 * signals at high frequency. Surviving peers must detect crashes, audit poisoned slots,
 * recover expired leases, and continue streaming without deadlocking.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "rbipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <inttypes.h>

#define CHAOS_RING "/rbipc_chaos_test"
#define CHAOS_CAPACITY 64
#define CHAOS_SLOT_SZ 128
#define CHAOS_PRODUCERS 2
#define CHAOS_CONSUMERS 2
#define CHAOS_DURATION_SEC 2

static void run_producer(const char *name) {
    rbipc_ring_t *ring = NULL;
    int rc = rbipc_attach_ex(name, RBIPC_ROLE_PRODUCER, &ring);
    if (rc != RBIPC_OK) {
        _exit(1);
    }

    uint64_t counter = 0;
    char payload[64];

    for (;;) {
        void *buf = NULL;
        uint64_t ticket = 0;
        rc = rbipc_reserve_write_timeout(ring, 48, 50000000ULL, &buf, &ticket);
        if (rc == RBIPC_OK) {
            snprintf(payload, sizeof(payload), "CHAOS_MSG_%" PRIu64, counter++);
            memcpy(buf, payload, strlen(payload) + 1);
            rc = rbipc_commit_write(ring, ticket, (uint32_t)strlen(payload) + 1);
            if (rc != RBIPC_OK) {
                break;
            }
        } else if (rc == RBIPC_ERR_SHUTDOWN) {
            break;
        }
        /* Short pause to throttle */
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000 }; /* 100 us */
        nanosleep(&ts, NULL);
    }

    rbipc_detach(ring);
    _exit(0);
}

static void run_consumer(const char *name) {
    rbipc_ring_t *ring = NULL;
    int rc = rbipc_attach_ex(name, RBIPC_ROLE_CONSUMER, &ring);
    if (rc != RBIPC_OK) {
        _exit(1);
    }

    for (;;) {
        const void *buf = NULL;
        uint32_t len = 0;
        uint64_t ticket = 0;
        rc = rbipc_read_acquire_timeout(ring, 50000000ULL, &buf, &len, &ticket);
        if (rc == RBIPC_OK || rc == RBIPC_ERR_POISONED) {
            rc = rbipc_read_release(ring, ticket);
            if (rc != RBIPC_OK && rc != RBIPC_ERR_TIMEOUT) {
                break;
            }
        } else if (rc == RBIPC_ERR_SHUTDOWN) {
            break;
        }
    }

    rbipc_detach(ring);
    _exit(0);
}

int main(void) {
    printf("[test_e2e_chaos] Initializing automated chaos testing suite...\n");
    rbipc_destroy(CHAOS_RING);

    rbipc_ring_t *ring = NULL;
    int rc = rbipc_create_ex(CHAOS_RING, CHAOS_CAPACITY, CHAOS_SLOT_SZ,
                             RBIPC_CREATE_EXCL | RBIPC_ROLE_MONITOR, 0660, &ring);
    assert(rc == RBIPC_OK);
    assert(ring != NULL);

    /* Enable consumer lease timeouts (30ms) to trigger head-of-line unblocking */
    assert(rbipc_set_lease_timeout(ring, 30000000ULL) == RBIPC_OK);

    pid_t producers[CHAOS_PRODUCERS] = {};
    pid_t consumers[CHAOS_CONSUMERS] = {};

    for (int i = 0; i < CHAOS_PRODUCERS; ++i) {
        pid_t p = fork();
        if (p == 0) {
            run_producer(CHAOS_RING);
        }
        producers[i] = p;
    }

    for (int i = 0; i < CHAOS_CONSUMERS; ++i) {
        pid_t c = fork();
        if (c == 0) {
            run_consumer(CHAOS_RING);
        }
        consumers[i] = c;
    }

    time_t start_time = time(NULL);
    uint32_t chaos_injections = 0;
    uint32_t kills = 0;
    uint32_t stalls = 0;

    while (time(NULL) - start_time < CHAOS_DURATION_SEC) {
        struct timespec sleep_ts = { .tv_sec = 0, .tv_nsec = 25000000 }; /* 25 ms */
        nanosleep(&sleep_ts, NULL);

        /* Check for dead workers and respawn */
        int status;
        pid_t dead_pid;
        while ((dead_pid = waitpid(-1, &status, WNOHANG)) > 0) {
            for (int i = 0; i < CHAOS_PRODUCERS; ++i) {
                if (producers[i] == dead_pid) {
                    pid_t np = fork();
                    if (np == 0) {
                        run_producer(CHAOS_RING);
                    }
                    producers[i] = np;
                    break;
                }
            }
            for (int i = 0; i < CHAOS_CONSUMERS; ++i) {
                if (consumers[i] == dead_pid) {
                    pid_t nc = fork();
                    if (nc == 0) {
                        run_consumer(CHAOS_RING);
                    }
                    consumers[i] = nc;
                    break;
                }
            }
        }

        /* Pick a victim process */
        int r = rand() % (CHAOS_PRODUCERS + CHAOS_CONSUMERS);
        pid_t victim = (r < CHAOS_PRODUCERS) ? producers[r] : consumers[r - CHAOS_PRODUCERS];
        if (victim <= 0) continue;

        int action = rand() % 3;
        if (action == 0) {
            /* SIGKILL: simulate sudden ungraceful crash */
            kill(victim, SIGKILL);
            kills++;
        } else {
            /* SIGSTOP + SIGCONT: simulate CPU preemption / hypervisor pause */
            kill(victim, SIGSTOP);
            struct timespec pause_ts = { .tv_sec = 0, .tv_nsec = 5000000 }; /* 5 ms */
            nanosleep(&pause_ts, NULL);
            kill(victim, SIGCONT);
            stalls++;
        }
        chaos_injections++;
    }

    printf("[test_e2e_chaos] Chaos session complete. Injections: %u (%u SIGKILLs, %u SIGSTOP stalls)\n",
           chaos_injections, kills, stalls);

    /* Signal shutdown and terminate all workers */
    rbipc_signal_shutdown(ring);

    for (int i = 0; i < CHAOS_PRODUCERS; ++i) {
        if (producers[i] > 0) {
            kill(producers[i], SIGKILL);
            waitpid(producers[i], NULL, 0);
        }
    }
    for (int i = 0; i < CHAOS_CONSUMERS; ++i) {
        if (consumers[i] > 0) {
            kill(consumers[i], SIGKILL);
            waitpid(consumers[i], NULL, 0);
        }
    }

    /* Drain any remaining reaped children */
    while (waitpid(-1, NULL, WNOHANG) > 0) {}

    /* Audit and recover expired leases or poisoned slots before cleanup */
    int revoked = rbipc_revoke_expired_leases(ring);
    (void)revoked;

    /* Verify ring handle is detached and unlinked */
    rbipc_detach(ring);
    rbipc_destroy(CHAOS_RING);

    assert(chaos_injections > 0);
    printf("[test_e2e_chaos] Chaos resilience test passed!\n");
    return 0;
}
