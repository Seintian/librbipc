#!/usr/bin/env python3
import subprocess
import threading
import time
import glob
import os
import json
import sys

class HardwareMonitor:
    def __init__(self, interval_sec=0.25):
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
        temps = [self._read_int(f) // 1000 for f in temp_files]
        core_th = sum(self._read_int(f) for f in core_throttles)
        pkg_th = sum(self._read_int(f) for f in pkg_throttles)
        bat_status = self._read_str("/sys/class/power_supply/BAT0/status")
        bat_cap = self._read_int("/sys/class/power_supply/BAT0/capacity")

        return {
            "time": time.time(),
            "freqs_mhz": freqs,
            "avg_freq_mhz": sum(freqs) / len(freqs) if freqs else 0,
            "min_freq_mhz": min(freqs) if freqs else 0,
            "max_freq_mhz": max(freqs) if freqs else 0,
            "temps_c": temps,
            "max_temp_c": max(temps) if temps else 0,
            "core_throttle_count": core_th,
            "pkg_throttle_count": pkg_th,
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

def run_bench(bench_bin, bench_name, env_override=None):
    print(f"\n=======================================================")
    print(f" Starting Benchmark: {bench_name} ({bench_bin})")
    print(f"=======================================================")
    
    monitor = HardwareMonitor(interval_sec=0.2)
    initial_snap = monitor.snapshot()
    monitor.start()

    start_time = time.time()
    env = os.environ.copy()
    if env_override:
        env.update(env_override)

    proc = subprocess.Popen([bench_bin], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env)
    stdout, _ = proc.communicate()
    elapsed = time.time() - start_time

    samples = monitor.stop()
    final_snap = monitor.snapshot()

    print(stdout)

    if proc.returncode != 0:
        print(f"ERROR: {bench_name} failed with return code {proc.returncode}")
        sys.exit(proc.returncode)

    # Analyze samples
    if samples:
        all_avg_freqs = [s["avg_freq_mhz"] for s in samples]
        all_max_temps = [s["max_temp_c"] for s in samples]
        min_freq = min(all_avg_freqs)
        max_freq = max(all_avg_freqs)
        mean_freq = sum(all_avg_freqs) / len(all_avg_freqs)
        peak_temp = max(all_max_temps)
    else:
        min_freq = initial_snap["avg_freq_mhz"]
        max_freq = initial_snap["avg_freq_mhz"]
        mean_freq = initial_snap["avg_freq_mhz"]
        peak_temp = initial_snap["max_temp_c"]

    core_th_delta = final_snap["core_throttle_count"] - initial_snap["core_throttle_count"]
    pkg_th_delta = final_snap["pkg_throttle_count"] - initial_snap["pkg_throttle_count"]

    telemetry = {
        "benchmark": bench_name,
        "binary": bench_bin,
        "elapsed_sec": round(elapsed, 2),
        "min_freq_mhz": round(min_freq, 1),
        "max_freq_mhz": round(max_freq, 1),
        "mean_freq_mhz": round(mean_freq, 1),
        "initial_temp_c": initial_snap["max_temp_c"],
        "peak_temp_c": peak_temp,
        "final_temp_c": final_snap["max_temp_c"],
        "core_throttle_delta": core_th_delta,
        "pkg_throttle_delta": pkg_th_delta,
        "battery_capacity_start": initial_snap["battery_capacity"],
        "battery_capacity_end": final_snap["battery_capacity"],
    }

    print(f"\n--- [Hardware Monitor: {bench_name}] ---")
    print(f"Elapsed Time     : {elapsed:.2f} s")
    print(f"CPU Frequency    : Mean {mean_freq:.1f} MHz (Range: {min_freq:.1f} - {max_freq:.1f} MHz)")
    print(f"Temperature      : Initial {initial_snap['max_temp_c']}°C -> Peak {peak_temp}°C -> Final {final_snap['max_temp_c']}°C")
    print(f"Throttle Events  : Core Throttle Δ={core_th_delta}, Package Throttle Δ={pkg_th_delta}")
    print(f"Battery Capacity : {initial_snap['battery_capacity']}% -> {final_snap['battery_capacity']}%")

    return telemetry

def main():
    benchmarks = [
        ("bin/bench_suite", "bench_suite"),
        ("bin/bench_deep_dive", "bench_deep_dive"),
        ("bin/bench_idle_strategies", "bench_idle_strategies"),
        ("bin/bench_competitors", "bench_competitors"),
    ]

    all_telemetry = []
    for bbin, bname in benchmarks:
        tel = run_bench(bbin, bname)
        all_telemetry.append(tel)

    os.makedirs("benchmarks/results/laptop_powersave", exist_ok=True)
    with open("benchmarks/results/laptop_powersave/hardware_throttling_telemetry.json", "w") as f:
        json.dump(all_telemetry, f, indent=2)

    print("\n=======================================================")
    print(" ALL BENCHMARKS COMPLETED SUCCESSFULLY")
    print(" Telemetry written to benchmarks/results/laptop_powersave/hardware_throttling_telemetry.json")
    print("=======================================================")

if __name__ == "__main__":
    main()
