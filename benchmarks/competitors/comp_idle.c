/**
 * @file comp_idle.c
 * @brief Idle CPU consumption benchmark for competitors.
 */

#include "comp_common.h"

void run_bench_idle_cpu(FILE *json_fp) {
    comp_print_hdr("BENCH 4: Idle Consumer CPU Utilisation -- 400ms Window (Passive vs Busy)");

    const uint64_t IDLE_NS = 400000000ULL;
    double cpu_rte = 0, cpu_dis = 0, cpu_mq = 0, cpu_uds = 0, cpu_rbi = 0;

    /* rte_ring: busy-poll */
    {
        rte_ring_t *idle_r = rte_ring_alloc(RING_CAPACITY);
        struct rusage ru0, ru1;
        getrusage(RUSAGE_SELF, &ru0);
        uint64_t t0 = comp_get_ns();
        uintptr_t v;
        while (comp_get_ns() - t0 < IDLE_NS) rte_mc_dequeue(idle_r, &v);
        getrusage(RUSAGE_SELF, &ru1);
        cpu_rte = comp_calc_idle_cpu(&ru0, &ru1, comp_get_ns() - t0);
        printf("\n  %-25s  CPU Idle: %s%5.1f%%%s  (busy-spin -- DPDK poll-mode)\n",
               "rte_ring (DPDK alg)", COL_RED, cpu_rte, COL_RESET);
        rte_ring_free(idle_r);
    }

    /* Disruptor: busy-poll */
    {
        disruptor_ring_t *idle_d = disruptor_alloc(RING_CAPACITY);
        struct rusage ru0, ru1;
        getrusage(RUSAGE_SELF, &ru0);
        uint64_t t0 = comp_get_ns();
        uintptr_t v;
        while (comp_get_ns() - t0 < IDLE_NS) disruptor_consume_wp(idle_d, 0, &v);
        getrusage(RUSAGE_SELF, &ru1);
        cpu_dis = comp_calc_idle_cpu(&ru0, &ru1, comp_get_ns() - t0);
        printf("  %-25s  CPU Idle: %s%5.1f%%%s  (busy-spin -- sequence gating)\n",
               "LMAX Disruptor", COL_RED, cpu_dis, COL_RESET);
        disruptor_free(idle_d);
    }

    /* POSIX mqueue: non-blocking spin */
    {
        mqd_t imq = mq_make(MQ_NAME_AB, 8, 10);
        struct mq_attr a = { .mq_flags = O_NONBLOCK, .mq_maxmsg = 10, .mq_msgsize = 8, .mq_curmsgs = 0 };
        mq_setattr(imq, &a, NULL);
        struct rusage ru0, ru1;
        getrusage(RUSAGE_SELF, &ru0);
        uint64_t t0 = comp_get_ns();
        char buf[16]; unsigned int p;
        while (comp_get_ns() - t0 < IDLE_NS) mq_receive(imq, buf, 8, &p);
        getrusage(RUSAGE_SELF, &ru1);
        cpu_mq = comp_calc_idle_cpu(&ru0, &ru1, comp_get_ns() - t0);
        printf("  %-25s  CPU Idle: %s%5.1f%%%s  (non-blocking spin)\n",
               "POSIX mqueue", COL_YELLOW, cpu_mq, COL_RESET);
        mq_close(imq); mq_unlink(MQ_NAME_AB);
    }

    /* AF_UNIX: non-blocking recv */
    {
        int sv[2];
        assert(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0);
        int fl = fcntl(sv[1], F_GETFL);
        fcntl(sv[1], F_SETFL, fl | O_NONBLOCK);
        struct rusage ru0, ru1;
        getrusage(RUSAGE_SELF, &ru0);
        uint64_t t0 = comp_get_ns();
        char buf[16];
        while (comp_get_ns() - t0 < IDLE_NS) recv(sv[1], buf, 8, 0);
        getrusage(RUSAGE_SELF, &ru1);
        cpu_uds = comp_calc_idle_cpu(&ru0, &ru1, comp_get_ns() - t0);
        printf("  %-25s  CPU Idle: %s%5.1f%%%s  (non-blocking recv)\n",
               "AF_UNIX DGRAM", COL_YELLOW, cpu_uds, COL_RESET);
        close(sv[0]); close(sv[1]);
    }

    /* librbipc: futex passive wait */
    {
        rbipc_ring_t *ir = NULL;
        rbipc_destroy("/rbench_idle");
        if (rbipc_create_ex("/rbench_idle", RING_CAPACITY, 72, RBIPC_SHM_NAMED | RBIPC_FLAG_TRUSTED, 0660, &ir) == RBIPC_OK) {
            struct rusage ru0, ru1;
            getrusage(RUSAGE_SELF, &ru0);
            uint64_t t0 = comp_get_ns();
            while (comp_get_ns() - t0 < IDLE_NS) {
                const void *buf; uint64_t tk; uint32_t ln;
                int rto = rbipc_read_acquire_timeout(ir, 10000000ULL, &buf, &ln, &tk);
                (void)rto;
            }
            getrusage(RUSAGE_SELF, &ru1);
            cpu_rbi = comp_calc_idle_cpu(&ru0, &ru1, comp_get_ns() - t0);
            printf("  %-25s  CPU Idle: %s%5.1f%%%s  (futex passive sleep)\n",
               "librbipc", COL_GREEN, cpu_rbi, COL_RESET);
            rbipc_detach(ir);
            rbipc_destroy("/rbench_idle");
        }
    }

    if (json_fp) {
        fprintf(json_fp, "  \"idle_cpu\": {\n");
        fprintf(json_fp, "    \"rte_ring\": %.1f,\n    \"disruptor\": %.1f,\n    \"mqueue\": %.1f,\n    \"af_unix\": %.1f,\n    \"librbipc\": %.1f\n",
                cpu_rte, cpu_dis, cpu_mq, cpu_uds, cpu_rbi);
        fprintf(json_fp, "  },\n");
    }
}
