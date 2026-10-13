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
 * @brief Detect current hardware profile ("laptop" or "home_pc").
 */
static inline const char *bench_detect_profile(void) {
    const char *env = getenv("RBIPC_BENCH_PROFILE");
    if (env && env[0] != '\0') {
        return env;
    }

    char host[64] = {0};
    if (gethostname(host, sizeof(host) - 1) == 0) {
        if (strstr(host, "LAPTOP") || strstr(host, "laptop")) {
            return "laptop";
        }
    }

    long procs = sysconf(_SC_NPROCESSORS_ONLN);
    if (procs > 0 && procs <= 4) {
        return "laptop";
    }

    FILE *f = fopen("/proc/cpuinfo", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (strstr(line, "model name") && (strstr(line, "6300U") || strstr(line, "Mobile") || strstr(line, "Laptop"))) {
                fclose(f);
                return "laptop";
            }
        }
        fclose(f);
    }

    return "home_pc";
}

/**
 * @brief Populate hardware information dynamically for JSON telemetry.
 */
static inline void bench_get_hardware_info(bench_hardware_info_t *info) {
    memset(info, 0, sizeof(*info));
    info->profile_name = bench_detect_profile();
    info->is_laptop = (strcmp(info->profile_name, "laptop") == 0);

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
    if (strcmp(profile, "laptop") == 0) {
        return laptop_iters;
    }
    return default_iters;
}

/**
 * @brief Yielding spin backoff to prevent CFS thread starvation during thread oversubscription.
 */
static inline void bench_spin_pause(uint32_t *spin_count) {
    if (++(*spin_count) < 32) {
#if defined(__x86_64__) || defined(_M_X64)
        __asm__ volatile("pause" ::: "memory");
#elif defined(__aarch64__)
        __asm__ volatile("isb" ::: "memory");
#else
        __asm__ volatile("" ::: "memory");
#endif
    } else if (*spin_count < 64) {
        sched_yield();
    } else {
        struct timespec ts = {0, 500};
        nanosleep(&ts, NULL);
        *spin_count = 0;
    }
}

#endif /* RBIPC_BENCH_COMMON_H */
