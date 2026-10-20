/**
 * @file opt_lifecycle.c
 * @brief Module 4: Anonymous Shared Memory vs Named POSIX Shm Lifecycle.
 */

#include "arch_common.h"

void run_module4_lifecycle(uint64_t cycles, m4_lifecycle_result_t *out) {
    arch_print_subhdr("Module 4: Anonymous Shared Memory vs Named POSIX Shm Lifecycle");
    printf("Cycles: %lu (Create -> Attach/Pass -> Detach -> Destroy)\n\n", (unsigned long)cycles);

    /* 1. Named POSIX Shared Memory */
    uint64_t t0 = arch_get_time_ns();
    for (uint64_t i = 0; i < cycles; ++i) {
        rbipc_ring_t *ring = NULL;
        rbipc_ring_t *ring2 = NULL;
        assert(rbipc_create_ex(ARCH_SHM_LIFECYCLE, 64, 64, RBIPC_SHM_NAMED, 0660, &ring) == RBIPC_OK);
        assert(rbipc_attach(ARCH_SHM_LIFECYCLE, &ring2) == RBIPC_OK);
        rbipc_detach(ring2);
        rbipc_detach(ring);
        rbipc_destroy(ARCH_SHM_LIFECYCLE);
    }
    uint64_t t1 = arch_get_time_ns();

    /* 2. Anonymous Shared Memory via memfd + SCM_RIGHTS */
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    uint64_t t2 = arch_get_time_ns();
    for (uint64_t i = 0; i < cycles; ++i) {
        rbipc_ring_t *ring = NULL;
        rbipc_ring_t *ring2 = NULL;
        assert(rbipc_create_ex(NULL, 64, 64, RBIPC_SHM_ANON, 0660, &ring) == RBIPC_OK);
        assert(rbipc_send_fd(sv[0], ring) == RBIPC_OK);
        assert(rbipc_recv_fd(sv[1], &ring2) == RBIPC_OK);
        rbipc_detach(ring2);
        rbipc_detach(ring);
    }
    uint64_t t3 = arch_get_time_ns();

    close(sv[0]);
    close(sv[1]);

    double named_sec = (double)(t1 - t0) / 1e9;
    double anon_sec  = (double)(t3 - t2) / 1e9;

    out->named_ops_sec    = (double)cycles / named_sec;
    out->named_latency_us = (named_sec * 1e6) / (double)cycles;
    out->anon_ops_sec     = (double)cycles / anon_sec;
    out->anon_latency_us  = (anon_sec * 1e6) / (double)cycles;
    out->speedup_factor   = out->anon_ops_sec / out->named_ops_sec;

    printf("  Named POSIX SHM Lifecycle   : %8.2f cycles/sec | Latency: %6.2f us/cycle\n",
           out->named_ops_sec, out->named_latency_us);
    printf("  Anonymous memfd + SCM_RIGHTS: %8.2f cycles/sec | Latency: %6.2f us/cycle | Speedup: %.2fx\n\n",
           out->anon_ops_sec, out->anon_latency_us, out->speedup_factor);
}
