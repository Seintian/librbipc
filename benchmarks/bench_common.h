/**
 * @file bench_common.h
 * @brief Common benchmark utilities, hardware profiling, and directory routing for librbipc.
 */

#ifndef RBIPC_BENCH_COMMON_H
#define RBIPC_BENCH_COMMON_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/utsname.h>

typedef struct {
    char cpu_model[256];
    int physical_cores;
    int logical_threads;
    char kernel[256];
    char hostname[64];
    bool is_laptop;
    const char *profile_name;
} bench_hardware_info_t;

/**
 * @brief Check whether system is currently running on battery (power-save mode).
 */
static inline bool bench_is_on_battery(void) {
    FILE *f = fopen("/sys/class/power_supply/AC/online", "r");
    if (f) {
        int online = 1;
        if (fscanf(f, "%d", &online) == 1) {
            fclose(f);
            return (online == 0);
        }
        fclose(f);
    }
    f = fopen("/sys/class/power_supply/BAT0/status", "r");
    if (f) {
        char status[32] = {0};
        if (fgets(status, sizeof(status), f)) {
            fclose(f);
            if (strstr(status, "Discharging")) {
                return true;
            }
        } else {
            fclose(f);
        }
    }
    return false;
}

/**
 * @brief Detect current hardware profile ("laptop_powersave", "laptop", or "home_pc").
 */
static inline const char *bench_detect_profile(void) {
    const char *env = getenv("RBIPC_BENCH_PROFILE");
    if (env && env[0] != '\0') {
        return env;
    }

    bool is_laptop_hw = false;
    char host[64] = {0};
    if (gethostname(host, sizeof(host) - 1) == 0) {
        if (strstr(host, "LAPTOP") || strstr(host, "laptop")) {
            is_laptop_hw = true;
        }
    }

    if (!is_laptop_hw) {
        long procs = sysconf(_SC_NPROCESSORS_ONLN);
        if (procs > 0 && procs <= 4) {
            is_laptop_hw = true;
        }
    }

    if (!is_laptop_hw) {
        FILE *f = fopen("/proc/cpuinfo", "r");
        if (f) {
            char line[256];
            while (fgets(line, sizeof(line), f)) {
                if (strstr(line, "model name") && (strstr(line, "6300U") || strstr(line, "Mobile") || strstr(line, "Laptop"))) {
                    is_laptop_hw = true;
                    break;
                }
            }
            fclose(f);
        }
    }

    if (is_laptop_hw) {
        if (bench_is_on_battery()) {
            return "laptop_powersave";
        }
        return "laptop";
    }

    return "home_pc";
}

/**
 * @brief Populate hardware information dynamically for JSON telemetry.
 */
static inline void bench_get_hardware_info(bench_hardware_info_t *info) {
    memset(info, 0, sizeof(*info));
    info->profile_name = bench_detect_profile();
    info->is_laptop = (strncmp(info->profile_name, "laptop", 6) == 0);

    if (gethostname(info->hostname, sizeof(info->hostname) - 1) != 0) {
        strncpy(info->hostname, "unknown", sizeof(info->hostname) - 1);
    }

    struct utsname un;
    if (uname(&un) == 0) {
        snprintf(info->kernel, sizeof(info->kernel), "%s %s %s", un.sysname, un.release, un.machine);
    } else {
        strncpy(info->kernel, "Linux (unknown)", sizeof(info->kernel) - 1);
    }

    long online_cpus = sysconf(_SC_NPROCESSORS_ONLN);
    info->logical_threads = (online_cpus > 0) ? (int)online_cpus : 1;
    info->physical_cores = (info->logical_threads > 1) ? (info->logical_threads / 2) : 1;

    strncpy(info->cpu_model, "Generic x86_64 Processor", sizeof(info->cpu_model) - 1);
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "model name", 10) == 0) {
                char *colon = strchr(line, ':');
                if (colon) {
                    colon++;
                    while (*colon == ' ' || *colon == '\t') colon++;
                    char *nl = strchr(colon, '\n');
                    if (nl) *nl = '\0';
                    strncpy(info->cpu_model, colon, sizeof(info->cpu_model) - 1);
                    break;
                }
            }
        }
        fclose(f);
    }
}

/**
 * @brief Build destination file path in benchmarks/results/<profile>/<filename>.
 */
static inline void bench_get_result_path(char *out_path, size_t max_len, const char *filename) {
    const char *profile = bench_detect_profile();
    mkdir("benchmarks", 0755);
    mkdir("benchmarks/results", 0755);

    char dir[128];
    snprintf(dir, sizeof(dir), "benchmarks/results/%s", profile);
    mkdir(dir, 0755);

    snprintf(out_path, max_len, "benchmarks/results/%s/%s", profile, filename);
}

/**
 * @brief Scale iterations according to the active hardware profile.
 */
static inline uint64_t bench_scale_iterations(uint64_t default_iters, uint64_t laptop_iters) {
    const char *profile = bench_detect_profile();
    if (strncmp(profile, "laptop", 6) == 0) {
        return laptop_iters;
    }
    return default_iters;
}

/**
 * @brief Yielding spin backoff to prevent CFS thread starvation during thread oversubscription.
 */
static inline void bench_spin_pause(uint32_t *spin_count) {
    if (++(*spin_count) < 64) {
#if defined(__x86_64__) || defined(_M_X64)
        __asm__ volatile("pause" ::: "memory");
#elif defined(__aarch64__)
        __asm__ volatile("isb" ::: "memory");
#else
        __asm__ volatile("" ::: "memory");
#endif
    } else {
        sched_yield();
        *spin_count = 0;
    }
}

#endif /* RBIPC_BENCH_COMMON_H */
