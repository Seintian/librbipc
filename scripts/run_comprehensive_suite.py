#!/usr/bin/env python3
"""
Comprehensive Benchmark & Hardware Profiling Suite for librbipc.
Profiles all latest changes on host machine (Laptop AC Mode),
monitors CPU thermals, throttling, clock frequencies, captures Linux perf counters,
and computes exhaustive before-vs-after regression/speedup metrics.
"""

import subprocess
import threading
import time
import glob
import os
import json
import sys
import re

class HardwareMonitor:
    def __init__(self, interval_sec=0.2):
        self.interval = interval_sec
        self.running = False
        self.thread = None
        self.samples = []
        self.lock = threading.Lock()

    def _read_int(self, path):
        try:
            with open(path, "r") as f:
                return int(f.read().strip())
        except Exception:
            return 0

    def _read_str(self, path):
        try:
            with open(path, "r") as f:
                return f.read().strip()
        except Exception:
            return ""

    def snapshot(self):
        freq_files = sorted(glob.glob("/sys/devices/system/cpu/cpu*/cpufreq/scaling_cur_freq"))
        temp_files = sorted(glob.glob("/sys/class/thermal/thermal_zone*/temp"))
        core_throttles = sorted(glob.glob("/sys/devices/system/cpu/cpu*/thermal_throttle/core_throttle_count"))
        pkg_throttles = sorted(glob.glob("/sys/devices/system/cpu/cpu*/thermal_throttle/package_throttle_count"))

        freqs = [self._read_int(f) // 1000 for f in freq_files]
        temps = [self._read_int(f) // 1000 for f in temp_files if self._read_int(f) > 0]
        core_th = sum(self._read_int(f) for f in core_throttles)
        pkg_th = sum(self._read_int(f) for f in pkg_throttles)

        ac_online = self._read_int("/sys/class/power_supply/AC/online") == 1
        bat_status = self._read_str("/sys/class/power_supply/BAT0/status")
        bat_cap = self._read_int("/sys/class/power_supply/BAT0/capacity")

        return {
            "timestamp": time.time(),
            "freqs_mhz": freqs,
            "avg_freq_mhz": sum(freqs) / len(freqs) if freqs else 0.0,
            "min_freq_mhz": min(freqs) if freqs else 0.0,
            "max_freq_mhz": max(freqs) if freqs else 0.0,
            "temps_c": temps,
            "max_temp_c": max(temps) if temps else 0.0,
            "avg_temp_c": sum(temps) / len(temps) if temps else 0.0,
            "core_throttle_count": core_th,
            "pkg_throttle_count": pkg_th,
            "ac_online": ac_online,
            "battery_status": bat_status,
            "battery_capacity": bat_cap,
        }

    def _run(self):
        while self.running:
            s = self.snapshot()
            with self.lock:
                self.samples.append(s)
            time.sleep(self.interval)

    def start(self):
        self.running = True
        self.samples = []
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def stop(self):
        self.running = False
        if self.thread:
            self.thread.join(timeout=2.0)
        with self.lock:
            return list(self.samples)

def run_command_with_telemetry(cmd, label, env_override=None, capture_perf=False):
    print(f"\n{'='*80}")
    print(f" >>> RUNNING: {label}")
    print(f"     Command: {' '.join(cmd)}")
    print(f"{'='*80}")

    monitor = HardwareMonitor(interval_sec=0.15)
    init_snap = monitor.snapshot()
    monitor.start()

    env = os.environ.copy()
    env["RBIPC_BENCH_PROFILE"] = "laptop"
    if env_override:
        env.update(env_override)

    final_cmd = list(cmd)
    if capture_perf:
        perf_events = "cycles:u,instructions:u,branches:u,branch-misses:u,cache-references:u,cache-misses:u,context-switches:u,page-faults:u"
        final_cmd = ["perf", "stat", "-x,", "-e", perf_events] + final_cmd

    t0 = time.time()
    proc = subprocess.Popen(final_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
    stdout, stderr = proc.communicate()
    elapsed = time.time() - t0

    samples = monitor.stop()
    final_snap = monitor.snapshot()

    if stdout.strip():
        print(stdout)
    if not capture_perf and stderr.strip():
        print(stderr)

    if proc.returncode != 0:
        print(f"FAILED: {label} exited with return code {proc.returncode}")
        print(stderr)
        sys.exit(proc.returncode)

    perf_data = {}
    if capture_perf:
        # Parse CSV perf stats
        for line in stderr.splitlines():
            parts = line.split(",")
            if len(parts) >= 3 and parts[2].endswith(":u"):
                val_str = parts[0].strip()
                event = parts[2].strip()
                try:
                    val = float(val_str) if val_str and val_str != "<not counted>" else 0.0
                    perf_data[event] = val
                except ValueError:
                    pass

        # Compute derived metrics
        cycles = perf_data.get("cycles:u", 0.0)
        insns = perf_data.get("instructions:u", 0.0)
        branches = perf_data.get("branches:u", 0.0)
        b_misses = perf_data.get("branch-misses:u", 0.0)
        cache_refs = perf_data.get("cache-references:u", 0.0)
        cache_misses = perf_data.get("cache-misses:u", 0.0)

        ipc = (insns / cycles) if cycles > 0 else 0.0
        b_miss_pct = ((b_misses / branches) * 100.0) if branches > 0 else 0.0
        c_miss_pct = ((cache_misses / cache_refs) * 100.0) if cache_refs > 0 else 0.0

        perf_data["ipc"] = round(ipc, 2)
        perf_data["branch_miss_pct"] = round(b_miss_pct, 2)
        perf_data["cache_miss_pct"] = round(c_miss_pct, 2)

    # Compute telemetry summary
    avg_freqs = [s["avg_freq_mhz"] for s in samples] if samples else [init_snap["avg_freq_mhz"]]
    max_temps = [s["max_temp_c"] for s in samples] if samples else [init_snap["max_temp_c"]]

    tel = {
        "label": label,
        "elapsed_sec": round(elapsed, 4),
        "mean_freq_mhz": round(sum(avg_freqs) / len(avg_freqs), 1),
        "min_freq_mhz": round(min(avg_freqs), 1),
        "max_freq_mhz": round(max(avg_freqs), 1),
        "initial_temp_c": init_snap["max_temp_c"],
        "peak_temp_c": max(max_temps),
        "final_temp_c": final_snap["max_temp_c"],
        "core_throttle_delta": final_snap["core_throttle_count"] - init_snap["core_throttle_count"],
        "pkg_throttle_delta": final_snap["pkg_throttle_count"] - init_snap["pkg_throttle_count"],
        "ac_online": final_snap["ac_online"],
        "battery_capacity": final_snap["battery_capacity"],
        "perf_counters": perf_data
    }

    print(f"--- [Telemetry: {label}] ---")
    print(f"Elapsed: {elapsed:.2f}s | CPU Mean: {tel['mean_freq_mhz']} MHz | Temp: {tel['initial_temp_c']}°C -> Peak {tel['peak_temp_c']}°C | AC Online: {tel['ac_online']}")
    if capture_perf:
        print(f"Perf: IPC={tel['perf_counters'].get('ipc')} | Branch Miss Rate={tel['perf_counters'].get('branch_miss_pct')}% | Cache Miss Rate={tel['perf_counters'].get('cache_miss_pct')}%")

    return tel, stdout

def load_json(path):
    if not os.path.exists(path):
        return None
    try:
        with open(path, "r") as f:
            return json.load(f)
    except Exception as e:
        print(f"Error loading {path}: {e}")
        return None

def main():
    print("="*80)
    print("  LIBRBIPC COMPREHENSIVE BENCHMARK & HARDWARE PROFILING SUITE")
    print("  Host Target: Laptop (AC Mode)")
    print("="*80)

    # Ensure binary output directories and clean shm
    os.makedirs("benchmarks/results/laptop", exist_ok=True)
    subprocess.run(["rm", "-f", "/dev/shm/rbipc_*", "/dev/shm/rbench_*"], stderr=subprocess.DEVNULL)

    telemetry_records = []
    e2e_modes_output = ""

    # 1. Architectural Suite
    t, _ = run_command_with_telemetry(["bin/bench_architecture_suite"], "Architectural Evaluation Suite")
    telemetry_records.append(t)

    # 2. Core Zero-Spin & Scaling Suite
    t, _ = run_command_with_telemetry(["bin/bench_suite"], "Core Streaming & Latency Suite")
    telemetry_records.append(t)

    # 3. Deep Dive Features
    t, _ = run_command_with_telemetry(["bin/bench_deep_dive"], "Deep Dive Engine Suite")
    telemetry_records.append(t)

    # 4. Idle Strategies & CPU Wakeup
    t, _ = run_command_with_telemetry(["bin/bench_idle_strategies"], "Idle Strategies & Power Suite")
    telemetry_records.append(t)

    # 5. Competitor Comparative Analysis
    t, _ = run_command_with_telemetry(["bin/bench_competitors"], "Competitor Comparative Suite")
    telemetry_records.append(t)

    # 6. CRC32C Hardware SSE4.2 Microbenchmarks
    t, _ = run_command_with_telemetry(["bin/bench_crc32c"], "Hardware CRC32C Micro-Benchmark")
    telemetry_records.append(t)

    # 7. Isolated Profiling Harness (Pointer, Copy, Batch)
    t, _ = run_command_with_telemetry(["bin/bench_librbipc_isolated", "--ptr", "--iters", "2000000"],
                                      "Isolated Pointer Exchange (pthread)", capture_perf=True)
    telemetry_records.append(t)

    t, _ = run_command_with_telemetry(["bin/bench_librbipc_isolated", "--copy", "--iters", "2000000"],
                                      "Isolated 64B Copy Exchange (pthread)", capture_perf=True)
    telemetry_records.append(t)

    t, _ = run_command_with_telemetry(["bin/bench_librbipc_isolated", "--batch", "--iters", "5000000"],
                                      "Isolated B-Queue Batch 32 (pthread)", capture_perf=True)
    telemetry_records.append(t)

    # 8. End-to-End Architectural Execution Modes (Multi-Process Fork)
    t, out = run_command_with_telemetry(["bin/io/test_e2e_throughput"], "E2E Architectural Modes (Fork Multi-Process)", capture_perf=True)
    telemetry_records.append(t)
    e2e_modes_output = out

    # Parse e2e modes output
    mode_results = {}
    pattern = re.compile(r"===\s*librbipc\s+(?:Throughput|B-Queue Batch)\s+Benchmark:\s*(.*?)\s*===\s*\n"
                         r"Total Messages\s*:\s*(\d+)\s*\n"
                         r"Elapsed Time\s*:\s*([0-9.]+)\s*seconds\s*\n"
                         r"Throughput\s*:\s*([0-9.]+)\s*msgs/sec\s*\(([0-9.]+)\s*MB/sec\)\s*\n"
                         r"Average Latency:\s*([0-9.]+)\s*ns/message", re.MULTILINE)
    for m in pattern.finditer(e2e_modes_output):
        m_name = m.group(1).strip()
        mode_results[m_name] = {
            "total_messages": int(m.group(2)),
            "elapsed_sec": float(m.group(3)),
            "msgs_per_sec": float(m.group(4)),
            "mb_per_sec": float(m.group(5)),
            "avg_latency_ns": float(m.group(6)),
        }

    # Save comprehensive telemetry JSON
    output_bundle = {
        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "host_profile": "laptop",
        "power_mode": "AC Online",
        "hardware_telemetry": telemetry_records,
        "architectural_modes_e2e": mode_results,
    }

    out_json_path = "benchmarks/results/laptop/comprehensive_profiling_telemetry.json"
    with open(out_json_path, "w") as f:
        json.dump(output_bundle, f, indent=2)
    print(f"\n[INFO] Comprehensive telemetry exported to {out_json_path}")

    # 9. Perform Comprehensive Comparative Analysis vs Pre-Oct 22 Baseline
    baseline_dir = "benchmarks/results/.backup_laptop_pre_oct22"
    current_dir = "benchmarks/results/laptop"

    print("\n" + "="*95)
    print(" COMPREHENSIVE BEFORE-VS-AFTER REGRESSION & SCALING COMPARISON")
    print(f" Target Profile: Laptop (AC Mode) | Baseline: Pre-Oct 22 Snapshot")
    print("="*95)

    # A. Streaming & Latency Comparison
    old_suite = load_json(os.path.join(baseline_dir, "zero_spin_optimized.json"))
    new_suite = load_json(os.path.join(current_dir, "zero_spin_optimized.json"))

    if old_suite and new_suite:
        s_old = old_suite.get("streaming", {})
        s_new = new_suite.get("streaming", {})
        print("\n--- [1. Streaming Throughput & Latency] ---")
        print(f"{'Metric':<30} | {'Pre-Oct 22':<15} | {'Current (Oct 22)':<18} | {'Delta (%)':<15}")
        print("-" * 85)
        for label, k, mult, higher_better in [
            ("Throughput (Mops/s)", "msgs_per_sec", 1e-6, True),
            ("Bandwidth (MB/s)", "mb_per_sec", 1.0, True),
            ("Average Latency (ns)", "avg_latency_ns", 1.0, False)
        ]:
            ov = s_old.get(k, 0.0) * mult
            nv = s_new.get(k, 0.0) * mult
            delta = ((nv - ov) / ov * 100.0) if ov > 0 else 0.0
            flag = "✓ IMPROVED" if (delta > 0 if higher_better else delta < 0) else "✗ REGRESSED"
            print(f"{label:<30} | {ov:<15.2f} | {nv:<18.2f} | {delta:>+6.1f}% {flag}")

        l_old = old_suite.get("latency", {})
        l_new = new_suite.get("latency", {})
        print("\n--- [2. Half-RTT Latency Distribution] ---")
        print(f"{'Percentile':<30} | {'Pre-Oct 22 (ns)':<15} | {'Current (ns)':<18} | {'Delta (%)':<15}")
        print("-" * 85)
        for p in ["min_ns", "mean_ns", "p50_ns", "p90_ns", "p99_ns", "p99_9_ns", "max_ns"]:
            ov = l_old.get(p, 0.0)
            nv = l_new.get(p, 0.0)
            delta = ((nv - ov) / ov * 100.0) if ov > 0 else 0.0
            flag = "✓ FASTER" if delta < 0 else "✗ SLOWER"
            print(f"{p:<30} | {ov:<15.1f} | {nv:<18.1f} | {delta:>+6.1f}% {flag}")

        # Batching scaling comparison
        b_old = {item["size"]: item for item in old_suite.get("batching", [])}
        b_new = {item["size"]: item for item in new_suite.get("batching", [])}
        print("\n--- [3. B-Queue Vector Batching Scaling (Mops/s)] ---")
        print(f"{'Batch Size':<30} | {'Pre-Oct 22':<15} | {'Current (Oct 22)':<18} | {'Delta (%)':<15}")
        print("-" * 85)
        for bsz in sorted(b_new.keys()):
            ov = (b_old.get(bsz, {}).get("msgs_per_sec", 0.0)) / 1e6
            nv = (b_new[bsz].get("msgs_per_sec", 0.0)) / 1e6
            delta = ((nv - ov) / ov * 100.0) if ov > 0 else 0.0
            flag = "✓ FASTER" if delta > 0 else "✗ SLOWER"
            print(f"Batch Size {bsz:<19} | {ov:<15.2f} | {nv:<18.2f} | {delta:>+6.1f}% {flag}")

    # B. Competitor Comparison
    old_comp = load_json(os.path.join(baseline_dir, "competitors_benchmark.json"))
    new_comp = load_json(os.path.join(current_dir, "competitors_benchmark.json"))
    if new_comp:
        print("\n--- [4. Competitor Comparative Analysis (Throughput & Speedup)] ---")
        print(f"{'Transport / IPC Mechanism':<30} | {'Throughput (Mops/s)':<20} | {'Latency (ns)':<15} | {'Speedup vs POSIX SHM':<20}")
        print("-" * 90)
        c_items = new_comp.get("competitors", [])
        rbipc_th = 0.0
        shm_th = 0.0
        for item in c_items:
            if "librbipc" in item.get("name", "").lower():
                rbipc_th = item.get("throughput_mops", 0.0)
            if "posix shared memory" in item.get("name", "").lower():
                shm_th = item.get("throughput_mops", 0.0)

        for item in c_items:
            name = item.get("name", "")
            th = item.get("throughput_mops", 0.0)
            lat = item.get("latency_ns", 0.0)
            ratio = f"{(th / shm_th):.2f}x" if shm_th > 0 else "1.00x"
            print(f"{name:<30} | {th:<20.2f} | {lat:<15.1f} | {ratio:<20}")

    # C. Architectural Modes Comparison
    if mode_results:
        print("\n--- [5. In-Tree Architectural Modes Evaluation (Multi-Process)] ---")
        print(f"{'Architectural Mode':<45} | {'Throughput (Mops/s)':<20} | {'Bandwidth (MB/s)':<18} | {'Avg Latency (ns)':<15}")
        print("-" * 105)
        for m_name, data in mode_results.items():
            print(f"{m_name:<45} | {data['msgs_per_sec']/1e6:<20.2f} | {data['mb_per_sec']:<18.2f} | {data['avg_latency_ns']:<15.2f}")

    print("\n" + "="*95)
    print(" ALL BENCHMARKS & PROFILING RUNS SUCCESSFULLY COMPLETED!")
    print("="*95 + "\n")

if __name__ == "__main__":
    main()
