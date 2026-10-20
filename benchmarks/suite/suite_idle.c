/**
 * @file suite_idle.c
 * @brief Idle / backpressure waiting CPU utilization and context switches benchmark
 */

#include "suite_common.h"

void run_bench_idle(idle_metrics_t *out_metrics) {
    printf("\n[1/6] Running Idle / Backpressure Waiting CPU Test (2.0s observation)...\n");
    rbipc_destroy(BENCH_SHM_A);

    rbipc_ring_t *ring = NULL;
    int rc = rbipc_create(BENCH_SHM_A, 256, 64, &ring);
    assert(rc == RBIPC_OK);

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        perror("pipe");
        exit(1);
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(1);
    }

    if (pid == 0) {
        /* Child: Consumer waiting passively on empty ring */
        close(pipefd[0]);
        rbipc_ring_t *c_ring = NULL;
        rc = rbipc_attach(BENCH_SHM_A, &c_ring);
        assert(rc == RBIPC_OK);

        const void *buf = NULL;
        uint32_t len = 0;
        uint64_t ticket = 0;

        /* Will block until shutdown */
        rc = rbipc_read_acquire(c_ring, &buf, &len, &ticket);
        assert(rc == RBIPC_ERR_SHUTDOWN);

        struct rusage ru;
        getrusage(RUSAGE_SELF, &ru);

        idle_metrics_t m;
        m.user_sec = (double)ru.ru_utime.tv_sec + (double)ru.ru_utime.tv_usec / 1e6;
        m.sys_sec  = (double)ru.ru_stime.tv_sec + (double)ru.ru_stime.tv_usec / 1e6;
        m.total_sec = m.user_sec + m.sys_sec;
        m.voluntary_ctxt_switches = ru.ru_nvcsw;
        m.involuntary_ctxt_switches = ru.ru_nivcsw;

        ssize_t w = write(pipefd[1], &m, sizeof(m));
        (void)w;
        close(pipefd[1]);
        rbipc_detach(c_ring);
        _exit(0);
    }

    /* Parent: Sleep for 2.0 seconds while child waits */
    close(pipefd[1]);
    struct timespec req = { .tv_sec = 2, .tv_nsec = 0 };
    nanosleep(&req, NULL);

    rbipc_signal_shutdown(ring);

    idle_metrics_t m;
    ssize_t r = read(pipefd[0], &m, sizeof(m));
    (void)r;
    close(pipefd[0]);
    waitpid(pid, NULL, 0);

    rbipc_detach(ring);
    rbipc_destroy(BENCH_SHM_A);

    m.cpu_pct = (m.total_sec / 2.0) * 100.0;
    *out_metrics = m;

    printf("  User CPU Time   : %.4f s\n", m.user_sec);
    printf("  System CPU Time : %.4f s\n", m.sys_sec);
    printf("  Total CPU Used  : %.4f s (%.2f%% core usage)\n", m.total_sec, m.cpu_pct);
    printf("  Voluntary CS    : %ld\n", m.voluntary_ctxt_switches);
    printf("  Involuntary CS  : %ld\n", m.involuntary_ctxt_switches);
}
