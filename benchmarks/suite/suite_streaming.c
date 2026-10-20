/**
 * @file suite_streaming.c
 * @brief High-throughput streaming IPC benchmark (1P - 1C)
 */

#include "suite_common.h"

void run_bench_streaming(int iterations, stream_metrics_t *out_metrics) {
    printf("\n[3/6] Running High-Throughput Streaming IPC (%d messages, 64-byte payload)...\n", iterations);
    rbipc_destroy(BENCH_SHM_A);

    rbipc_ring_t *ring = NULL;
    assert(rbipc_create(BENCH_SHM_A, 2048, 128, &ring) == RBIPC_OK);

    int pipefd[2];
    assert(pipe(pipefd) == 0);

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(1);
    }

    if (pid == 0) {
        /* Producer Child */
        close(pipefd[0]);
        rbipc_ring_t *p_ring = NULL;
        assert(rbipc_attach(BENCH_SHM_A, &p_ring) == RBIPC_OK);

        char payload[64];
        memset(payload, 0x42, sizeof(payload));

        for (int i = 0; i < iterations; ++i) {
            void *buf = NULL;
            uint64_t ticket = 0;
            int rc = rbipc_reserve_write(p_ring, sizeof(payload), &buf, &ticket);
            assert(rc == RBIPC_OK);
            *(uint32_t *)buf = (uint32_t)i;
            rc = rbipc_commit_write(p_ring, ticket, sizeof(payload));
            assert(rc == RBIPC_OK);
        }

        rbipc_signal_shutdown(p_ring);

        struct rusage ru;
        getrusage(RUSAGE_SELF, &ru);
        long cs[2] = { ru.ru_nvcsw, ru.ru_nivcsw };
        ssize_t w = write(pipefd[1], cs, sizeof(cs));
        (void)w;
        close(pipefd[1]);

        rbipc_detach(p_ring);
        _exit(0);
    }

    /* Consumer Parent */
    close(pipefd[1]);
    uint64_t t0 = get_time_ns();
    int count = 0;

    while (count < iterations) {
        const void *buf = NULL;
        uint32_t len = 0;
        uint64_t ticket = 0;
        int rc = rbipc_read_acquire(ring, &buf, &len, &ticket);
        if (rc == RBIPC_ERR_SHUTDOWN) break;
        assert(rc == RBIPC_OK);
        count++;
        rbipc_read_release(ring, ticket);
    }

    uint64_t t1 = get_time_ns();
    double elapsed = (double)(t1 - t0) / 1e9;

    long prod_cs[2];
    ssize_t r = read(pipefd[0], prod_cs, sizeof(prod_cs));
    (void)r;
    close(pipefd[0]);
    waitpid(pid, NULL, 0);

    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);

    out_metrics->iterations = count;
    out_metrics->elapsed_sec = elapsed;
    out_metrics->msgs_per_sec = (double)count / elapsed;
    out_metrics->mb_per_sec = (out_metrics->msgs_per_sec * 64.0) / (1024.0 * 1024.0);
    out_metrics->avg_latency_ns = (elapsed * 1e9) / (double)count;
    out_metrics->producer_nvcsw = prod_cs[0];
    out_metrics->producer_nivcsw = prod_cs[1];
    out_metrics->consumer_nvcsw = ru.ru_nvcsw;
    out_metrics->consumer_nivcsw = ru.ru_nivcsw;

    rbipc_detach(ring);
    rbipc_destroy(BENCH_SHM_A);

    printf("  Messages Handled: %d in %.4f s\n", count, elapsed);
    printf("  Throughput      : %.2f msgs/sec (%.2f MB/sec)\n", out_metrics->msgs_per_sec, out_metrics->mb_per_sec);
    printf("  Average Latency : %.2f ns/msg\n", out_metrics->avg_latency_ns);
    printf("  Producer CS     : %ld vol, %ld invol\n", out_metrics->producer_nvcsw, out_metrics->producer_nivcsw);
    printf("  Consumer CS     : %ld vol, %ld invol\n", out_metrics->consumer_nvcsw, out_metrics->consumer_nivcsw);
}
