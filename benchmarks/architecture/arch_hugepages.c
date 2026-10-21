/**
 * @file opt_hugepages.c
 * @brief Module 8: Zero-Config Dynamic HugePages vs Standard 4KB Paging.
 */

#include "arch_common.h"

static unsigned long get_smaps_pmd_mapped_kb(void) {
    FILE *f = fopen("/proc/self/smaps", "r");
    if (!f) return 0;
    char line[256];
    bool in_shm = false;
    unsigned long kb = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "/memfd:rbipc")) {
            in_shm = true;
        } else if (in_shm) {
            unsigned long v = 0;
            if (sscanf(line, "ShmemPmdMapped: %lu kB", &v) == 1) {
                kb += v;
            }
            if (strstr(line, "VmFlags:")) {
                in_shm = false;
            }
        }
    }
    fclose(f);
    return kb;
}

void run_module8_dynamic_hugepages(uint64_t iterations,
                                   m8_dynamic_hugepages_result_t out_results[4]) {
    arch_print_banner("Module 8: Zero-Config Dynamic HugePages vs Standard 4KB Paging");
    printf("Evaluating memory latency, streaming bandwidth, and TLB efficiency across ring buffer sizes...\n\n");

    struct {
        const char *label;
        uint32_t capacity;
        uint32_t slot_size;
    } configs[4] = {
        {"Small Queue (64 KB)",       1024,   64},
        {"Medium Ring (4 MB)",        1024, 4096},
        {"Large Ring (16 MB)",        4096, 4096},
        {"Very Large Ring (32 MB)",   8192, 4096}
    };

    printf("%s%-26s %-16s %-16s %-16s %-10s%s\n",
           COL_BOLD, "Buffer Configuration", "Standard 4KB", "Dynamic 2MB THP", "ShmemPmdMapped", "Speedup", COL_RESET);
    printf("----------------------------------------------------------------------------------------------------\n");

    for (int c = 0; c < 4; ++c) {
        uint32_t cap = configs[c].capacity;
        uint32_t slot_sz = configs[c].slot_size;
        double buf_mb = (double)((uint64_t)cap * slot_sz) / (1024.0 * 1024.0);
        uint64_t iters = (buf_mb >= 16.0) ? (iterations / 2) : iterations;
        if (iters < 50000ULL) iters = 50000ULL;

        /* 1. Dynamic Transparent HugePages */
        rbipc_ring_t *ring_thp = NULL;
        int rc = rbipc_create(NULL, cap, slot_sz, &ring_thp);
        assert(rc == RBIPC_OK && ring_thp != NULL);

        /* Touch data buffer to populate PMD entries */
        for (uint32_t i = 0; i < cap; ++i) {
            void *wbuf = NULL; uint64_t wt = 0;
            if (rbipc_reserve_write(ring_thp, 64, &wbuf, &wt) == RBIPC_OK) {
                memset(wbuf, 0xAA, 64);
                rbipc_commit_write(ring_thp, wt, 64);
            }
        }
        for (uint32_t i = 0; i < cap; ++i) {
            const void *rbuf = NULL; uint32_t rl = 0; uint64_t rt = 0;
            if (rbipc_read_acquire(ring_thp, &rbuf, &rl, &rt) == RBIPC_OK) {
                rbipc_read_release(ring_thp, rt);
            }
        }
        unsigned long thp_pmd_kb = get_smaps_pmd_mapped_kb();

        uint64_t t0 = arch_get_time_ns();
        for (uint64_t i = 0; i < iters; ++i) {
            void *wbuf = NULL; uint64_t wt = 0;
            if (rbipc_reserve_write(ring_thp, 64, &wbuf, &wt) == RBIPC_OK) {
                memset(wbuf, (int)(i & 0xFF), 64);
                rbipc_commit_write(ring_thp, wt, 64);
            }
            const void *rbuf = NULL; uint32_t rl = 0; uint64_t rt = 0;
            if (rbipc_read_acquire(ring_thp, &rbuf, &rl, &rt) == RBIPC_OK) {
                rbipc_read_release(ring_thp, rt);
            }
        }
        uint64_t t1 = arch_get_time_ns();
        double sec_thp = (double)(t1 - t0) / 1e9;
        double thp_mps = (double)iters / sec_thp;
        double thp_lat = (sec_thp * 1e9) / (double)iters;
        double thp_mb  = (thp_mps * 64.0) / (1024.0 * 1024.0);
        rbipc_detach(ring_thp);

        /* 2. Standard 4KB Paging (MADV_NOHUGEPAGE fallback) */
        rbipc_ring_t *ring_std = NULL;
        rc = rbipc_create(NULL, cap, slot_sz, &ring_std);
        assert(rc == RBIPC_OK && ring_std != NULL);

        rbipc_stats_t stats;
        rc = rbipc_get_stats(ring_std, &stats);
        assert(rc == RBIPC_OK);

        void *wbuf_probe = NULL; uint64_t wt_probe = 0;
        rc = rbipc_reserve_write(ring_std, 64, &wbuf_probe, &wt_probe);
        assert(rc == RBIPC_OK);
        uintptr_t data_base = (uintptr_t)wbuf_probe & ~((uintptr_t)stats.data_size - 1);
#ifdef MADV_NOHUGEPAGE
        madvise((void *)data_base, stats.data_size, MADV_NOHUGEPAGE);
#endif
#ifdef MADV_DONTNEED
        madvise((void *)data_base, stats.data_size, MADV_DONTNEED);
#endif
        rbipc_commit_write(ring_std, wt_probe, 64);
        const void *rbuf_probe = NULL; uint32_t rl_probe = 0; uint64_t rt_probe = 0;
        rc = rbipc_read_acquire(ring_std, &rbuf_probe, &rl_probe, &rt_probe);
        assert(rc == RBIPC_OK);
        rbipc_read_release(ring_std, rt_probe);

        uint64_t t2 = arch_get_time_ns();
        for (uint64_t i = 0; i < iters; ++i) {
            void *wbuf = NULL; uint64_t wt = 0;
            if (rbipc_reserve_write(ring_std, 64, &wbuf, &wt) == RBIPC_OK) {
                memset(wbuf, (int)(i & 0xFF), 64);
                rbipc_commit_write(ring_std, wt, 64);
            }
            const void *rbuf = NULL; uint32_t rl = 0; uint64_t rt = 0;
            if (rbipc_read_acquire(ring_std, &rbuf, &rl, &rt) == RBIPC_OK) {
                rbipc_read_release(ring_std, rt);
            }
        }
        uint64_t t3 = arch_get_time_ns();
        double sec_std = (double)(t3 - t2) / 1e9;
        double std_mps = (double)iters / sec_std;
        double std_lat = (sec_std * 1e9) / (double)iters;
        double std_mb  = (std_mps * 64.0) / (1024.0 * 1024.0);
        rbipc_detach(ring_std);

        double speedup = thp_mps / std_mps;
        double lat_reduct = ((std_lat - thp_lat) / std_lat) * 100.0;
        if (lat_reduct < 0.0) lat_reduct = 0.0;

        out_results[c].label = configs[c].label;
        out_results[c].capacity = cap;
        out_results[c].slot_size = slot_sz;
        out_results[c].buffer_mb = buf_mb;
        out_results[c].thp_msgs_sec = thp_mps;
        out_results[c].thp_mb_sec = thp_mb;
        out_results[c].thp_lat_ns = thp_lat;
        out_results[c].thp_pmd_mapped_kb = thp_pmd_kb;
        out_results[c].std_msgs_sec = std_mps;
        out_results[c].std_mb_sec = std_mb;
        out_results[c].std_lat_ns = std_lat;
        out_results[c].speedup_factor = speedup;
        out_results[c].latency_reduction_pct = lat_reduct;

        char std_str[64];
        char thp_str[64];
        char pmd_str[64];
        snprintf(std_str, sizeof(std_str), "%.2f M/s (%4.0f ns)", std_mps / 1e6, std_lat);
        snprintf(thp_str, sizeof(thp_str), "%.2f M/s (%4.0f ns)", thp_mps / 1e6, thp_lat);
        snprintf(pmd_str, sizeof(pmd_str), "%lu kB", thp_pmd_kb);

        printf("%-26s %-16s %-16s %-16s %s%.2fx (%+.1f%%)%s\n",
               configs[c].label, std_str, thp_str, pmd_str,
               (speedup >= 1.05) ? COL_GREEN : COL_RESET,
               speedup, lat_reduct, COL_RESET);
    }
    printf("\n");
}
