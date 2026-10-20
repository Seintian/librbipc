/**
 * @file bench_idle_strategies.c
 * @brief Master coordinator for non-CPU-burning wait strategies benchmark.
 * @details Evaluates DPDK rte_ring, LMAX Disruptor, and librbipc wait strategies.
 */

#include "idle/idle_common.h"

uint64_t g_tput_iters   = 100000ULL;
uint64_t g_tput_warmup  = 1000ULL;
uint64_t g_lat_iters    = 10000ULL;
uint64_t g_wake_iters   = 100ULL;
uint64_t g_idle_obs_ns  = 1000000000ULL;
uint64_t g_wake_gap_ns  = 15000000ULL;

int main(void) {
    bench_hardware_info_t hw;
    bench_get_hardware_info(&hw);

    g_tput_iters  = bench_scale_iterations(100000ULL, 25000ULL);
    g_tput_warmup = bench_scale_iterations(1000ULL, 250ULL);
    g_lat_iters   = bench_scale_iterations(10000ULL, 2500ULL);
    g_wake_iters  = bench_scale_iterations(100ULL, 25ULL);
    g_idle_obs_ns = bench_scale_iterations(1000000000ULL, 250000000ULL);
    g_wake_gap_ns = bench_scale_iterations(15000000ULL, 5000000ULL);

    printf("%s%s========================================================================================%s\n",
           COL_BOLD, COL_CYAN, COL_RESET);
    printf("%s%s   INVESTIGATION: DPDK & LMAX DISRUPTOR IDLE & WAIT STRATEGIES BENCHMARK   %s\n",
           COL_BOLD, COL_YELLOW, COL_RESET);
    printf("%s   [Hardware Profile: %s%s%s | CPU: %s | %d cores / %d threads]\n",
           COL_CYAN, COL_BOLD, hw.profile_name, COL_CYAN, hw.cpu_model, hw.physical_cores, hw.logical_threads);
    printf("%s%s========================================================================================%s\n\n",
           COL_BOLD, COL_CYAN, COL_RESET);
    fflush(stdout);

    uint64_t *lat_buf  = (uint64_t *)malloc(g_lat_iters * sizeof(uint64_t));
    uint64_t *wake_buf = (uint64_t *)malloc(g_wake_iters * sizeof(uint64_t));
    assert(lat_buf && wake_buf);

    idle_bench_result_t results[10];
    size_t n_res = 0;

    printf("%s--- Benchmarking DPDK rte_ring Strategies ---%s\n", COL_BOLD, COL_RESET);
    fflush(stdout);
    {
        /* 1. DPDK BusySpin */
        dpdk_ring_t *r = dpdk_ring_alloc(RING_CAPACITY);
        dpdk_ring_t *r_ba = dpdk_ring_alloc(RING_CAPACITY);
        dpdk_ring_reset(r);
        double tput = run_dpdk_tput(r, 0);
        dpdk_ring_reset(r); dpdk_ring_reset(r_ba);
        run_dpdk_lat(r, r_ba, 0, lat_buf);
        dpdk_ring_reset(r);
        run_dpdk_wake(r, 0, wake_buf);
        dpdk_ring_reset(r);
        long vol = 0, invol = 0;
        double cpu = measure_dpdk_idle_cpu(r, 0, &vol, &invol);
        idle_record_res(results, &n_res, "DPDK-BusySpin", "DPDK Polling", tput, lat_buf, wake_buf, cpu, vol, invol);
        dpdk_ring_free(r);
        dpdk_ring_free(r_ba);

        /* 2. DPDK PauseLoop */
        r = dpdk_ring_alloc(RING_CAPACITY);
        r_ba = dpdk_ring_alloc(RING_CAPACITY);
        dpdk_ring_reset(r);
        tput = run_dpdk_tput(r, 1);
        dpdk_ring_reset(r); dpdk_ring_reset(r_ba);
        run_dpdk_lat(r, r_ba, 1, lat_buf);
        dpdk_ring_reset(r);
        run_dpdk_wake(r, 1, wake_buf);
        dpdk_ring_reset(r);
        cpu = measure_dpdk_idle_cpu(r, 1, &vol, &invol);
        idle_record_res(results, &n_res, "DPDK-PauseLoop", "DPDK Power", tput, lat_buf, wake_buf, cpu, vol, invol);
        dpdk_ring_free(r);
        dpdk_ring_free(r_ba);

        /* 3. DPDK Interrupt-Epoll */
        r = dpdk_ring_alloc(RING_CAPACITY);
        r_ba = dpdk_ring_alloc(RING_CAPACITY);
        dpdk_ring_reset(r);
        tput = run_dpdk_tput(r, 2);
        dpdk_ring_reset(r); dpdk_ring_reset(r_ba);
        run_dpdk_lat(r, r_ba, 2, lat_buf);
        dpdk_ring_reset(r);
        run_dpdk_wake(r, 2, wake_buf);
        dpdk_ring_reset(r);
        cpu = measure_dpdk_idle_cpu(r, 2, &vol, &invol);
        idle_record_res(results, &n_res, "DPDK-Interrupt-Epoll", "DPDK Interrupt", tput, lat_buf, wake_buf, cpu, vol, invol);
        dpdk_ring_free(r);
        dpdk_ring_free(r_ba);
    }

    printf("\n%s--- Benchmarking LMAX Disruptor Strategies ---%s\n", COL_BOLD, COL_RESET);
    fflush(stdout);
    {
        const struct {
            const char *name;
            const char *cat;
            lmax_strat_t strat;
        } disr_strats[6] = {
            { "LMAX-BusySpin",      "Disruptor Spin",   LMAX_STRAT_BUSYSPIN },
            { "LMAX-Yielding",      "Disruptor Yield",  LMAX_STRAT_YIELDING },
            { "LMAX-Sleeping",      "Disruptor Sleep",  LMAX_STRAT_SLEEPING },
            { "LMAX-Blocking",      "Disruptor Mutex",  LMAX_STRAT_BLOCKING },
            { "LMAX-LiteBlocking",  "Disruptor Lite",   LMAX_STRAT_LITEBLOCKING },
            { "LMAX-PhasedBackoff", "Disruptor Phased", LMAX_STRAT_PHASEDBACKOFF }
        };

        for (int i = 0; i < 6; i++) {
            lmax_ring_t *d = lmax_ring_alloc(RING_CAPACITY);
            lmax_ring_t *d_ba = lmax_ring_alloc(RING_CAPACITY);
            lmax_ring_reset(d);
            double tput = run_lmax_tput(d, disr_strats[i].strat);
            lmax_ring_reset(d); lmax_ring_reset(d_ba);
            run_lmax_lat(d, d_ba, disr_strats[i].strat, lat_buf);
            lmax_ring_reset(d);
            run_lmax_wake(d, disr_strats[i].strat, wake_buf);
            lmax_ring_reset(d);
            long vol = 0, invol = 0;
            double cpu = measure_lmax_idle_cpu(d, disr_strats[i].strat, &vol, &invol);
            idle_record_res(results, &n_res, disr_strats[i].name, disr_strats[i].cat,
                            tput, lat_buf, wake_buf, cpu, vol, invol);
            lmax_ring_free(d);
            lmax_ring_free(d_ba);
        }
    }

    printf("\n%s--- Benchmarking librbipc Strategy ---%s\n", COL_BOLD, COL_RESET);
    fflush(stdout);
    {
        /* 10. librbipc Futex */
        const char *ab_name = "/rbench_idle_ab";
        const char *ba_name = "/rbench_idle_ba";
        rbipc_env_t env = rbipc_env_init(ab_name, ba_name);
        double tput = run_rbipc_tput(&env);
        rbipc_env_cleanup(&env, ab_name, ba_name);

        env = rbipc_env_init(ab_name, ba_name);
        run_rbipc_lat(&env, lat_buf);
        rbipc_env_cleanup(&env, ab_name, ba_name);

        env = rbipc_env_init(ab_name, ba_name);
        run_rbipc_wake(&env, wake_buf);
        rbipc_env_cleanup(&env, ab_name, ba_name);

        env = rbipc_env_init(ab_name, ba_name);
        long vol = 0, invol = 0;
        double cpu = measure_rbipc_idle_cpu(&env, &vol, &invol);
        idle_record_res(results, &n_res, "librbipc-Futex", "Adaptive Futex", tput, lat_buf, wake_buf, cpu, vol, invol);
        rbipc_env_cleanup(&env, ab_name, ba_name);
    }

    /* Print Comparison Table */
    idle_print_table(results, n_res);

    /* Export JSON */
    char json_path[256];
    bench_get_result_path(json_path, sizeof(json_path), "idle_strategies_benchmark.json");
    idle_export_json(json_path, &hw, results, n_res);

    free(lat_buf);
    free(wake_buf);
    return 0;
}
