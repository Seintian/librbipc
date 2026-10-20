/**
 * @file opt_capabilities.c
 * @brief Module 7: Multi-FD SCM_RIGHTS Transmission, Futex2 Memoization, and HugeTLB Status.
 */

#include "arch_common.h"

void run_module7_primitives(uint64_t fd_iterations, uint64_t futex_iterations,
                            m7_primitives_result_t *out) {
    arch_print_subhdr("Module 7: Multi-FD SCM_RIGHTS, Futex2 Memoization, and HugeTLB");
    printf("Multi-FD Iterations: %lu | Futex Probes: %lu\n\n",
           (unsigned long)fd_iterations, (unsigned long)futex_iterations);

    /* 1. Multi-FD transmission */
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    int src_fds[4];
    for (int i = 0; i < 4; ++i) {
        src_fds[i] = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        assert(src_fds[i] >= 0);
    }

    /* 1.1 Single call with 1 FD */
    uint64_t t0 = arch_get_time_ns();
    for (uint64_t i = 0; i < fd_iterations; ++i) {
        int rfd = -1;
        assert(rbipc_ipc_send_fds(sv[0], &src_fds[0], 1) == RBIPC_OK);
        assert(rbipc_ipc_recv_fd(sv[1], &rfd) == RBIPC_OK);
        close(rfd);
    }
    uint64_t t1 = arch_get_time_ns();
    double sec1 = (double)(t1 - t0) / 1e9;
    out->mfd_1_ops_sec = (double)fd_iterations / sec1;
    out->mfd_1_latency_us = (sec1 * 1e6) / (double)fd_iterations;

    /* 1.2 Single call with 2 FDs */
    uint64_t t2 = arch_get_time_ns();
    for (uint64_t i = 0; i < fd_iterations; ++i) {
        int rfds[2] = { -1, -1 };
        size_t count = 0;
        assert(rbipc_ipc_send_fds(sv[0], src_fds, 2) == RBIPC_OK);
        assert(rbipc_ipc_recv_fds(sv[1], rfds, 2, &count) == RBIPC_OK);
        assert(count == 2);
        close(rfds[0]);
        close(rfds[1]);
    }
    uint64_t t3 = arch_get_time_ns();
    double sec2 = (double)(t3 - t2) / 1e9;
    out->mfd_2_ops_sec = (double)fd_iterations / sec2;
    out->mfd_2_latency_us = (sec2 * 1e6) / (double)fd_iterations;

    /* 1.3 Single call with 4 FDs */
    uint64_t t4 = arch_get_time_ns();
    for (uint64_t i = 0; i < fd_iterations; ++i) {
        int rfds[4] = { -1, -1, -1, -1 };
        size_t count = 0;
        assert(rbipc_ipc_send_fds(sv[0], src_fds, 4) == RBIPC_OK);
        assert(rbipc_ipc_recv_fds(sv[1], rfds, 4, &count) == RBIPC_OK);
        assert(count == 4);
        close(rfds[0]);
        close(rfds[1]);
        close(rfds[2]);
        close(rfds[3]);
    }
    uint64_t t5 = arch_get_time_ns();
    double sec4 = (double)(t5 - t4) / 1e9;
    out->mfd_4_ops_sec = (double)fd_iterations / sec4;
    out->mfd_4_latency_us = (sec4 * 1e6) / (double)fd_iterations;

    /* 1.4 Sequential 4 single-FD calls */
    uint64_t t6 = arch_get_time_ns();
    for (uint64_t i = 0; i < fd_iterations; ++i) {
        for (int k = 0; k < 4; ++k) {
            int rfd = -1;
            assert(rbipc_ipc_send_fd(sv[0], src_fds[k]) == RBIPC_OK);
            assert(rbipc_ipc_recv_fd(sv[1], &rfd) == RBIPC_OK);
            close(rfd);
        }
    }
    uint64_t t7 = arch_get_time_ns();
    double sec_seq = (double)(t7 - t6) / 1e9;
    out->mfd_4_seq_ops_sec = (double)fd_iterations / sec_seq;
    out->mfd_4_seq_latency_us = (sec_seq * 1e6) / (double)fd_iterations;
    out->mfd_4_speedup = out->mfd_4_ops_sec / out->mfd_4_seq_ops_sec;

    for (int i = 0; i < 4; ++i) close(src_fds[i]);
    close(sv[0]);
    close(sv[1]);

    printf("  Multi-FD Transfer (1 FD)        : %8.2f xfers/s | %6.2f us\n",
           out->mfd_1_ops_sec, out->mfd_1_latency_us);
    printf("  Multi-FD Transfer (2 FDs batched): %8.2f xfers/s | %6.2f us\n",
           out->mfd_2_ops_sec, out->mfd_2_latency_us);
    printf("  Multi-FD Transfer (4 FDs batched): %8.2f xfers/s | %6.2f us | Speedup: %.2fx vs 4x sequential\n",
           out->mfd_4_ops_sec, out->mfd_4_latency_us, out->mfd_4_speedup);
    printf("  Multi-FD Transfer (4x sequential): %8.2f xfers/s | %6.2f us\n\n",
           out->mfd_4_seq_ops_sec, out->mfd_4_seq_latency_us);

    /* 2. Futex2 Memoization */
    uint64_t tf0 = arch_get_time_ns();
    bool memo_val = false;
    for (uint64_t i = 0; i < futex_iterations; ++i) {
        memo_val = rbipc_futex2_has_u64();
    }
    uint64_t tf1 = arch_get_time_ns();
    __asm__ volatile("" : "+r"(memo_val));

    double fsec_memo = (double)(tf1 - tf0) / 1e9;
    out->futex_memo_ops_sec = (double)futex_iterations / fsec_memo;
    out->futex_memo_lat_ns  = (fsec_memo * 1e9) / (double)futex_iterations;

    uint64_t tf2 = arch_get_time_ns();
    uint64_t dummy = 0;
    struct futex_waitv w = {
        .val = 1,
        .uaddr = (uintptr_t)&dummy,
        .flags = 2,
        .__reserved = 0,
    };
    long sys_ret = 0;
    for (uint64_t i = 0; i < futex_iterations; ++i) {
        sys_ret += syscall(SYS_futex_waitv, &w, 1, 0, NULL, CLOCK_MONOTONIC);
    }
    uint64_t tf3 = arch_get_time_ns();
    __asm__ volatile("" : "+r"(sys_ret));

    double fsec_sys = (double)(tf3 - tf2) / 1e9;
    out->futex_syscall_ops_sec = (double)futex_iterations / fsec_sys;
    out->futex_syscall_lat_ns  = (fsec_sys * 1e9) / (double)futex_iterations;
    out->futex_speedup         = out->futex_syscall_lat_ns / out->futex_memo_lat_ns;

    printf("  Futex2 Memoized Capability Probe: %10.2f checks/s | %6.2f ns/check | Speedup: %.1fx\n",
           out->futex_memo_ops_sec, out->futex_memo_lat_ns, out->futex_speedup);
    printf("  Raw Kernel sys_futex_waitv Probe : %10.2f checks/s | %6.2f ns/check\n\n",
           out->futex_syscall_ops_sec, out->futex_syscall_lat_ns);

    /* 3. HugeTLB 2MB */
    out->hugetlb_available = rbipc_shm_hugetlb_available();
    printf("  HugeTLB 2MB Anonymous Shm Status: %s%s%s (Kernel support: %s)\n\n",
           out->hugetlb_available ? COL_GREEN : COL_YELLOW,
           out->hugetlb_available ? "Available (MFD_HUGETLB | MFD_HUGE_2MB)" : "Unavailable (0 nr_hugepages / architecture unsupported)",
           COL_RESET,
           out->hugetlb_available ? "YES" : "NO");
}
