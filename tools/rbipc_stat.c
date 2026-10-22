/**
 * @file rbipc_stat.c
 * @brief Non-intrusive CLI telemetry monitor and inspection tool for librbipc ring buffers.
 *
 * @details Attaches to an active shared memory ring buffer using RBIPC_ROLE_MONITOR,
 * collecting operational statistics, waiter distributions, cumulative error metrics,
 * and slot histograms with zero performance interference on active producers or consumers.
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
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <errno.h>

#define COL_RESET   "\033[0m"
#define COL_BOLD    "\033[1m"
#define COL_RED     "\033[31m"
#define COL_GREEN   "\033[32m"
#define COL_YELLOW  "\033[33m"
#define COL_BLUE    "\033[34m"
#define COL_MAGENTA "\033[35m"
#define COL_CYAN    "\033[36m"

static volatile sig_atomic_t g_running = 1;

static void handle_sigint(int sig) {
    (void)sig;
    g_running = 0;
}

static void print_usage(const char *prog) {
    printf("Usage: %s <shm_name> [options]\n\n", prog);
    printf("Non-intrusive zero-cost telemetry monitor for librbipc shared-memory rings.\n\n");
    printf("Options:\n");
    printf("  -w, --watch <seconds>  Stream metrics continuously every N seconds (e.g. 1.0)\n");
    printf("  -j, --json             Output telemetry snapshot in JSON format\n");
    printf("  -p, --prometheus       Output telemetry in Prometheus / OpenMetrics exposition format\n");
    printf("  -s, --slots            Inspect and display per-slot state distribution histogram\n");
    printf("  -a, --audit            Execute proactive lease audit and report revoked leases\n");
    printf("  -h, --help             Display this help message and exit\n\n");
    printf("Examples:\n");
    printf("  %s /my_ring\n", prog);
    printf("  %s /my_ring --watch 0.5\n", prog);
    printf("  %s /my_ring --json\n", prog);
    printf("  %s /my_ring --prometheus\n", prog);
}

static void print_json(const char *name, const rbipc_stats_t *s, const rbipc_slot_stats_t *ss, const rbipc_ring_t *ring) {
    printf("{\n");
    printf("  \"name\": \"%s\",\n", name);
    printf("  \"modes\": {\n");
    printf("    \"is_trusted\": %s,\n", rbipc_ring_is_trusted(ring) ? "true" : "false");
    printf("    \"is_packed\": %s,\n", rbipc_ring_is_packed(ring) ? "true" : "false");
    printf("    \"is_hugetlb\": %s,\n", rbipc_ring_is_hugetlb(ring) ? "true" : "false");
    printf("    \"is_wait_free\": %s,\n", rbipc_ring_is_wait_free(ring) ? "true" : "false");
    printf("    \"is_isolated\": %s,\n", rbipc_ring_is_isolated(ring) ? "true" : "false");
    printf("    \"is_cxl\": %s\n", rbipc_ring_is_cxl(ring) ? "true" : "false");
    printf("  },\n");
    printf("  \"geometry\": {\n");
    printf("    \"total_shm_size_bytes\": %lu,\n", (unsigned long)s->total_shm_size);
    printf("    \"data_size_bytes\": %lu,\n", (unsigned long)s->data_size);
    printf("    \"capacity_slots\": %u,\n", s->capacity);
    printf("    \"slot_size_bytes\": %u\n", s->slot_size);
    printf("  },\n");
    printf("  \"progress\": {\n");
    printf("    \"write_ticket\": %lu,\n", (unsigned long)s->write_ticket);
    printf("    \"read_ticket\": %lu,\n", (unsigned long)s->read_ticket);
    printf("    \"backlog_slots\": %lu\n", (unsigned long)s->backlog);
    printf("  },\n");
    printf("  \"participants\": {\n");
    printf("    \"active_producers\": %u,\n", s->active_producers);
    printf("    \"active_consumers\": %u,\n", s->active_consumers);
    printf("    \"is_shutdown\": %s\n", s->is_shutdown ? "true" : "false");
    printf("  },\n");
    printf("  \"synchronization\": {\n");
    printf("    \"consumer_futex_waiters\": %u,\n", s->futex_waiters);
    printf("    \"producer_write_waiters\": %u,\n", s->write_waiters);
    printf("    \"eventfd_reactor_waiters\": %u\n", s->eventfd_waiters);
    printf("  },\n");
    printf("  \"lease\": {\n");
    printf("    \"lease_timeout_ns\": %lu\n", (unsigned long)s->lease_timeout_ns);
    printf("  },\n");
    printf("  \"recovery_counters\": {\n");
    printf("    \"poisoned_slots_count\": %lu,\n", (unsigned long)s->poisoned_slots_count);
    printf("    \"dead_producers_recovered\": %lu,\n", (unsigned long)s->dead_producers_recovered);
    printf("    \"dead_consumers_recovered\": %lu,\n", (unsigned long)s->dead_consumers_recovered);
    printf("    \"leases_revoked_count\": %lu,\n", (unsigned long)s->leases_revoked_count);
    printf("    \"ticket_holes_healed\": %lu\n", (unsigned long)s->ticket_holes_healed);
    printf("  }");
    if (ss) {
        printf(",\n  \"slots\": {\n");
        printf("    \"empty\": %u,\n", ss->empty_count);
        printf("    \"reserving\": %u,\n", ss->reserving_count);
        printf("    \"reserved\": %u,\n", ss->reserved_count);
        printf("    \"committed\": %u,\n", ss->committed_count);
        printf("    \"poisoned\": %u,\n", ss->poisoned_count);
        printf("    \"active_leased_slots\": %u,\n", ss->active_leased_slots);
        printf("    \"max_lease_elapsed_ns\": %lu\n", (unsigned long)ss->max_lease_elapsed_ns);
        printf("  }\n");
    } else {
        printf("\n");
    }
    printf("}\n");
}

static void print_prometheus(const char *name, const rbipc_stats_t *s, const rbipc_slot_stats_t *ss, const rbipc_ring_t *ring) {
    printf("# HELP rbipc_mode_active Boolean indicator of active architectural execution modes\n");
    printf("# TYPE rbipc_mode_active gauge\n");
    printf("rbipc_mode_active{ring=\"%s\",mode=\"trusted\"} %d\n", name, rbipc_ring_is_trusted(ring) ? 1 : 0);
    printf("rbipc_mode_active{ring=\"%s\",mode=\"packed\"} %d\n", name, rbipc_ring_is_packed(ring) ? 1 : 0);
    printf("rbipc_mode_active{ring=\"%s\",mode=\"hugetlb\"} %d\n", name, rbipc_ring_is_hugetlb(ring) ? 1 : 0);
    printf("rbipc_mode_active{ring=\"%s\",mode=\"wait_free\"} %d\n", name, rbipc_ring_is_wait_free(ring) ? 1 : 0);
    printf("rbipc_mode_active{ring=\"%s\",mode=\"isolated\"} %d\n", name, rbipc_ring_is_isolated(ring) ? 1 : 0);
    printf("rbipc_mode_active{ring=\"%s\",mode=\"cxl\"} %d\n\n", name, rbipc_ring_is_cxl(ring) ? 1 : 0);

    printf("# HELP rbipc_total_shm_size_bytes Total file size in bytes of shared memory ring\n");
    printf("# TYPE rbipc_total_shm_size_bytes gauge\n");
    printf("rbipc_total_shm_size_bytes{ring=\"%s\"} %lu\n\n", name, (unsigned long)s->total_shm_size);

    printf("# HELP rbipc_capacity_slots Total circular slot capacity\n");
    printf("# TYPE rbipc_capacity_slots gauge\n");
    printf("rbipc_capacity_slots{ring=\"%s\"} %u\n\n", name, s->capacity);

    printf("# HELP rbipc_write_ticket_total Cumulative monotonic write tickets reserved\n");
    printf("# TYPE rbipc_write_ticket_total counter\n");
    printf("rbipc_write_ticket_total{ring=\"%s\"} %lu\n\n", name, (unsigned long)s->write_ticket);

    printf("# HELP rbipc_read_ticket_total Cumulative monotonic read tickets acquired\n");
    printf("# TYPE rbipc_read_ticket_total counter\n");
    printf("rbipc_read_ticket_total{ring=\"%s\"} %lu\n\n", name, (unsigned long)s->read_ticket);

    printf("# HELP rbipc_backlog_slots Instantaneous pending unread message backlog\n");
    printf("# TYPE rbipc_backlog_slots gauge\n");
    printf("rbipc_backlog_slots{ring=\"%s\"} %lu\n\n", name, (unsigned long)s->backlog);

    printf("# HELP rbipc_active_participants Number of attached participant handles\n");
    printf("# TYPE rbipc_active_participants gauge\n");
    printf("rbipc_active_participants{ring=\"%s\",role=\"producer\"} %u\n", name, s->active_producers);
    printf("rbipc_active_participants{ring=\"%s\",role=\"consumer\"} %u\n\n", name, s->active_consumers);

    printf("# HELP rbipc_waiters Suspended threads waiting on synchronization primitives\n");
    printf("# TYPE rbipc_waiters gauge\n");
    printf("rbipc_waiters{ring=\"%s\",type=\"consumer_data\"} %u\n", name, s->futex_waiters);
    printf("rbipc_waiters{ring=\"%s\",type=\"producer_space\"} %u\n", name, s->write_waiters);
    printf("rbipc_waiters{ring=\"%s\",type=\"eventfd_reactor\"} %u\n\n", name, s->eventfd_waiters);

    printf("# HELP rbipc_recovery_events_total Cumulative crash recoveries and fault remediations\n");
    printf("# TYPE rbipc_recovery_events_total counter\n");
    printf("rbipc_recovery_events_total{ring=\"%s\",type=\"poisoned_slots\"} %lu\n", name, (unsigned long)s->poisoned_slots_count);
    printf("rbipc_recovery_events_total{ring=\"%s\",type=\"dead_producers\"} %lu\n", name, (unsigned long)s->dead_producers_recovered);
    printf("rbipc_recovery_events_total{ring=\"%s\",type=\"dead_consumers\"} %lu\n", name, (unsigned long)s->dead_consumers_recovered);
    printf("rbipc_recovery_events_total{ring=\"%s\",type=\"leases_revoked\"} %lu\n", name, (unsigned long)s->leases_revoked_count);
    printf("rbipc_recovery_events_total{ring=\"%s\",type=\"ticket_holes_healed\"} %lu\n\n", name, (unsigned long)s->ticket_holes_healed);

    if (ss) {
        printf("# HELP rbipc_slot_state_distribution Current slot count by state\n");
        printf("# TYPE rbipc_slot_state_distribution gauge\n");
        printf("rbipc_slot_state_distribution{ring=\"%s\",state=\"empty\"} %u\n", name, ss->empty_count);
        printf("rbipc_slot_state_distribution{ring=\"%s\",state=\"reserving\"} %u\n", name, ss->reserving_count);
        printf("rbipc_slot_state_distribution{ring=\"%s\",state=\"reserved\"} %u\n", name, ss->reserved_count);
        printf("rbipc_slot_state_distribution{ring=\"%s\",state=\"committed\"} %u\n", name, ss->committed_count);
        printf("rbipc_slot_state_distribution{ring=\"%s\",state=\"poisoned\"} %u\n", name, ss->poisoned_count);
    }
}

static void print_human(const char *name, const rbipc_stats_t *s, const rbipc_slot_stats_t *ss, const rbipc_ring_t *ring) {
    printf("%s================================================================================%s\n", COL_CYAN, COL_RESET);
    printf(" %slibrbipc Telemetry Monitor%s | Object: %s%s%s\n", COL_BOLD, COL_RESET, COL_GREEN, name, COL_RESET);
    printf("%s================================================================================%s\n", COL_CYAN, COL_RESET);

    printf("  %sArchitectural Execution Modes%s\n", COL_BOLD, COL_RESET);
    printf("    Tier 2 Trusted Mode : %s\n", rbipc_ring_is_trusted(ring) ? COL_GREEN "ACTIVE (Pure Vyukov Sequence)" COL_RESET : "NO");
    printf("    Tier 3 Packed Mode  : %s\n", rbipc_ring_is_packed(ring) ? COL_GREEN "ACTIVE (64-bit Descriptors)" COL_RESET : "NO");
    printf("    HugeTLB Backing     : %s\n", rbipc_ring_is_hugetlb(ring) ? COL_GREEN "ACTIVE (2MB HugePages)" COL_RESET : "NO");
    printf("    Wait-Free wCQ Engine: %s\n", rbipc_ring_is_wait_free(ring) ? COL_GREEN "ACTIVE (Nikolaev O(P) Helping)" COL_RESET : "NO");
    printf("    Isolated Zero-Trust : %s\n", rbipc_ring_is_isolated(ring) ? COL_GREEN "ACTIVE (Intel MPK + mseal)" COL_RESET : "NO");
    printf("    CXL Disaggregated   : %s\n", rbipc_ring_is_cxl(ring) ? COL_GREEN "ACTIVE (DAX + CLWB)" COL_RESET : "NO");
    printf("\n");

    printf("  %sRing Geometry%s\n", COL_BOLD, COL_RESET);
    printf("    Capacity            : %s%u slots%s\n", COL_YELLOW, s->capacity, COL_RESET);
    printf("    Max Payload / Slot  : %u bytes\n", s->slot_size);
    printf("    Single Circular Data: %lu bytes (%.2f KB)\n", (unsigned long)s->data_size, (double)s->data_size / 1024.0);
    printf("    Total Backing Size  : %lu bytes (%.2f KB)\n", (unsigned long)s->total_shm_size, (double)s->total_shm_size / 1024.0);
    printf("\n");

    printf("  %sOperational Progress & Backlog%s\n", COL_BOLD, COL_RESET);
    printf("    Write Ticket (Head) : %s%lu%s\n", COL_GREEN, (unsigned long)s->write_ticket, COL_RESET);
    printf("    Read Ticket (Tail)  : %s%lu%s\n", COL_BLUE, (unsigned long)s->read_ticket, COL_RESET);
    printf("    Current Backlog     : %s%lu slots%s (%.1f%% capacity)\n",
           s->backlog > 0 ? COL_YELLOW : COL_RESET,
           (unsigned long)s->backlog, COL_RESET,
           s->capacity > 0 ? ((double)s->backlog * 100.0 / (double)s->capacity) : 0.0);
    printf("\n");

    printf("  %sParticipant Concurrency & Waiters%s\n", COL_BOLD, COL_RESET);
    printf("    Active Producers    : %s%u%s\n", s->active_producers > 0 ? COL_GREEN : COL_RESET, s->active_producers, COL_RESET);
    printf("    Active Consumers    : %s%u%s\n", s->active_consumers > 0 ? COL_GREEN : COL_RESET, s->active_consumers, COL_RESET);
    printf("    Consumer Waiters    : %s%u%s (sleeping on data ready)\n", s->futex_waiters > 0 ? COL_MAGENTA : COL_RESET, s->futex_waiters, COL_RESET);
    printf("    Producer Waiters    : %s%u%s (sleeping on space ready)\n", s->write_waiters > 0 ? COL_MAGENTA : COL_RESET, s->write_waiters, COL_RESET);
    printf("    Eventfd Waiters     : %u (epoll / reactor listeners)\n", s->eventfd_waiters);
    printf("    Shutdown Signaled   : %s\n", s->is_shutdown ? COL_RED "YES" COL_RESET : COL_GREEN "NO" COL_RESET);
    printf("    Consumer Lease Limit: %lu ns (%lu ms)\n", (unsigned long)s->lease_timeout_ns, (unsigned long)(s->lease_timeout_ns / 1000000ULL));
    printf("\n");

    printf("  %sCumulative Fault Remediation & Recovery%s\n", COL_BOLD, COL_RESET);
    printf("    Poisoned Slots      : %s%lu%s\n", s->poisoned_slots_count > 0 ? COL_YELLOW : COL_RESET, (unsigned long)s->poisoned_slots_count, COL_RESET);
    printf("    Dead Producers Healed: %lu\n", (unsigned long)s->dead_producers_recovered);
    printf("    Dead Consumers Healed: %lu\n", (unsigned long)s->dead_consumers_recovered);
    printf("    Expired Leases Revoked: %lu\n", (unsigned long)s->leases_revoked_count);
    printf("    Ticket Holes Healed : %lu\n", (unsigned long)s->ticket_holes_healed);

    if (ss) {
        printf("\n");
        printf("  %sSlot Table State Distribution (C = %u)%s\n", COL_BOLD, ss->capacity, COL_RESET);
        printf("    EMPTY               : %s%u%s\n", COL_GREEN, ss->empty_count, COL_RESET);
        printf("    RESERVING           : %u\n", ss->reserving_count);
        printf("    RESERVED            : %u\n", ss->reserved_count);
        printf("    COMMITTED           : %s%u%s\n", COL_CYAN, ss->committed_count, COL_RESET);
        printf("    POISONED            : %s%u%s\n", ss->poisoned_count > 0 ? COL_RED : COL_RESET, ss->poisoned_count, COL_RESET);
        printf("    Active Leased Slots : %u (Max Elapsed Age: %lu ns)\n",
               ss->active_leased_slots, (unsigned long)ss->max_lease_elapsed_ns);
    }
    printf("%s================================================================================%s\n", COL_CYAN, COL_RESET);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    const char *shm_name = NULL;
    bool opt_json = false;
    bool opt_prometheus = false;
    bool opt_slots = true;
    bool opt_audit = false;
    double watch_sec = 0.0;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-j") == 0 || strcmp(argv[i], "--json") == 0) {
            opt_json = true;
        } else if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--prometheus") == 0) {
            opt_prometheus = true;
        } else if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--slots") == 0) {
            opt_slots = true;
        } else if (strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "--audit") == 0) {
            opt_audit = true;
        } else if ((strcmp(argv[i], "-w") == 0 || strcmp(argv[i], "--watch") == 0) && i + 1 < argc) {
            const char *arg = argv[++i];
            char *endptr = NULL;
            errno = 0;
            double val = strtod(arg, &endptr);
            if (errno != 0 || endptr == arg || *endptr != '\0' || val <= 0.0) {
                fprintf(stderr, "Error: Invalid watch interval '%s'. Expected positive number.\n", arg);
                return 1;
            }
            watch_sec = val;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        } else {
            shm_name = argv[i];
        }
    }

    if (!shm_name) {
        fprintf(stderr, "Error: shm_name argument is required.\n");
        return 1;
    }

    /* Attach as RBIPC_ROLE_MONITOR: completely zero-overhead observer */
    rbipc_ring_t *ring = NULL;
    int rc = rbipc_attach_ex(shm_name, RBIPC_ROLE_MONITOR, &ring);
    if (rc != RBIPC_OK) {
        fprintf(stderr, "Error: Failed to attach to shared memory ring '%s': %s (code %d)\n",
                shm_name, rbipc_strerror(rc), rc);
        return 2;
    }

    if (opt_audit) {
        int revoked = rbipc_revoke_expired_leases(ring);
        if (revoked < 0) {
            fprintf(stderr, "Audit warning: Lease revocation error: %s (code %d)\n",
                    rbipc_strerror(revoked), revoked);
        } else {
            printf("[rbipc_stat] Proactive lease audit executed: %d expired leases revoked.\n", revoked);
        }
    }

    struct sigaction sa = {};
    sa.sa_handler = handle_sigint;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    do {
        rbipc_stats_t stats;
        rc = rbipc_get_stats(ring, &stats);
        if (rc != RBIPC_OK) {
            fprintf(stderr, "Error: rbipc_get_stats failed: %s (code %d)\n",
                    rbipc_strerror(rc), rc);
            rbipc_detach(ring);
            return 3;
        }

        rbipc_slot_stats_t slot_stats;
        rbipc_slot_stats_t *p_ss = NULL;
        if (opt_slots) {
            if (rbipc_get_slot_stats(ring, &slot_stats) == RBIPC_OK) {
                p_ss = &slot_stats;
            }
        }

        if (opt_json) {
            print_json(shm_name, &stats, p_ss, ring);
        } else if (opt_prometheus) {
            print_prometheus(shm_name, &stats, p_ss, ring);
        } else {
            if (watch_sec > 0.0) {
                printf("\033[H\033[J"); /* Clear screen */
            }
            print_human(shm_name, &stats, p_ss, ring);
        }

        if (watch_sec > 0.0 && g_running) {
            struct timespec req = {
                .tv_sec = (time_t)watch_sec,
                .tv_nsec = (long)((watch_sec - (double)(time_t)watch_sec) * 1e9)
            };
            nanosleep(&req, NULL);
        }
    } while (watch_sec > 0.0 && g_running);

    rbipc_detach(ring);
    return 0;
}
