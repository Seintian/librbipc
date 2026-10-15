#!/usr/bin/env python3
import json
import os
import sys

def load_json(path):
    if not os.path.exists(path):
        return None
    try:
        with open(path, "r") as f:
            return json.load(f)
    except Exception as e:
        print(f"Error loading {path}: {e}")
        return None

def pct_diff(new_val, old_val):
    if old_val == 0 or old_val is None or new_val is None:
        return 0.0
    return ((new_val - old_val) / old_val) * 100.0

def compare_zero_spin():
    old_ps = load_json("benchmarks/results/.backup_prior_to_run/laptop_powersave/zero_spin_optimized.json")
    old_lap = load_json("benchmarks/results/.backup_prior_to_run/laptop/zero_spin_optimized.json")
    old_pc = load_json("benchmarks/results/.backup_prior_to_run/home_pc/zero_spin_optimized.json")
    new_ps = load_json("benchmarks/results/laptop_powersave/zero_spin_optimized.json")

    print("\n" + "="*95)
    print(" 1. ZERO-SPIN OPTIMIZED BENCHMARK COMPARISON")
    print("="*95)

    # Streaming
    s_new = new_ps.get("streaming", {})
    s_old_ps = old_ps.get("streaming", {}) if old_ps else {}
    s_old_lap = old_lap.get("streaming", {}) if old_lap else {}
    s_old_pc = old_pc.get("streaming", {}) if old_pc else {}

    print("\n--- [Streaming Throughput & Latency] ---")
    print(f"{'Metric':<30} | {'New (Bat Perf)':<15} | {'Old (Bat Save)':<15} | {'Old (AC Perf)':<15} | {'Old (Home PC)':<15} | {'vs Bat Save':<12}")
    print("-" * 115)
    
    metrics = [
        ("Throughput (Mops/s)", s_new.get("msgs_per_sec", 0)/1e6, s_old_ps.get("msgs_per_sec", 0)/1e6, s_old_lap.get("msgs_per_sec", 0)/1e6, s_old_pc.get("msgs_per_sec", 0)/1e6, True),
        ("Bandwidth (MB/s)", s_new.get("mb_per_sec", 0), s_old_ps.get("mb_per_sec", 0), s_old_lap.get("mb_per_sec", 0), s_old_pc.get("mb_per_sec", 0), True),
        ("Average Latency (ns)", s_new.get("avg_latency_ns", 0), s_old_ps.get("avg_latency_ns", 0), s_old_lap.get("avg_latency_ns", 0), s_old_pc.get("avg_latency_ns", 0), False),
    ]
    for label, nv, ov_ps, ov_lap, ov_pc, higher_is_better in metrics:
        diff_ps = pct_diff(nv, ov_ps)
        sign = "+" if diff_ps > 0 else ""
        better = (diff_ps > 0) if higher_is_better else (diff_ps < 0)
        flag = "✓ IMPROVED" if better else "✗ SLOWER"
        print(f"{label:<30} | {nv:<15.2f} | {ov_ps:<15.2f} | {ov_lap:<15.2f} | {ov_pc:<15.2f} | {sign}{diff_ps:>6.1f}% {flag}")

    # Latency Distribution
    l_new = new_ps.get("latency", {})
    l_old_ps = old_ps.get("latency", {}) if old_ps else {}
    l_old_lap = old_lap.get("latency", {}) if old_lap else {}
    l_old_pc = old_pc.get("latency", {}) if old_pc else {}

    print("\n--- [Latency Distribution (Half-RTT)] ---")
    print(f"{'Percentile':<30} | {'New (Bat Perf)':<15} | {'Old (Bat Save)':<15} | {'Old (AC Perf)':<15} | {'Old (Home PC)':<15} | {'vs Bat Save':<12}")
    print("-" * 115)
    for p in ["min_ns", "mean_ns", "p50_ns", "p90_ns", "p99_ns", "p99_9_ns", "max_ns"]:
        nv = l_new.get(p, 0)
        ov_ps = l_old_ps.get(p, 0)
        ov_lap = l_old_lap.get(p, 0)
        ov_pc = l_old_pc.get(p, 0)
        diff_ps = pct_diff(nv, ov_ps)
        sign = "+" if diff_ps > 0 else ""
        flag = "✓ FASTER" if diff_ps < 0 else "✗ SLOWER"
        print(f"{p:<30} | {nv:<15.1f} | {ov_ps:<15.1f} | {ov_lap:<15.1f} | {ov_pc:<15.1f} | {sign}{diff_ps:>6.1f}% {flag}")

    # Batching
    b_new = {item["size"]: item for item in new_ps.get("batching", [])}
    b_old_ps = {item["size"]: item for item in old_ps.get("batching", [])} if old_ps else {}
    b_old_lap = {item["size"]: item for item in old_lap.get("batching", [])} if old_lap else {}

    print("\n--- [B-Queue Batching Scaling (Throughput Mops/s)] ---")
    print(f"{'Batch Size':<30} | {'New (Bat Perf)':<15} | {'Old (Bat Save)':<15} | {'Old (AC Perf)':<15} | {'vs Bat Save':<12} | {'vs AC Perf':<12}")
    print("-" * 115)
    for bsz in sorted(b_new.keys()):
        nv = b_new[bsz]["msgs_per_sec"] / 1e6
        ov_ps = b_old_ps.get(bsz, {}).get("msgs_per_sec", 0) / 1e6
        ov_lap = b_old_lap.get(bsz, {}).get("msgs_per_sec", 0) / 1e6
        diff_ps = pct_diff(nv, ov_ps)
        diff_lap = pct_diff(nv, ov_lap)
        print(f"Batch Size {bsz:<19} | {nv:<15.2f} | {ov_ps:<15.2f} | {ov_lap:<15.2f} | {diff_ps:>+6.1f}%      | {diff_lap:>+6.1f}%")

    # MPMC Scaling
    m_new = {f"{item['p']}P-{item['c']}C": item for item in new_ps.get("mpmc", [])}
    m_old_ps = {f"{item['p']}P-{item['c']}C": item for item in old_ps.get("mpmc", [])} if old_ps else {}
    m_old_lap = {f"{item['p']}P-{item['c']}C": item for item in old_lap.get("mpmc", [])} if old_lap else {}

    print("\n--- [MPMC Contention Scaling (Throughput Mops/s)] ---")
    print(f"{'Contention (P-C)':<30} | {'New (Bat Perf)':<15} | {'Old (Bat Save)':<15} | {'Old (AC Perf)':<15} | {'vs Bat Save':<12} | {'vs AC Perf':<12}")
    print("-" * 115)
    for cfg in sorted(m_new.keys()):
        item = m_new[cfg]
        nv = item["msgs_per_sec"] / 1e6
        ov_ps = m_old_ps.get(cfg, {}).get("msgs_per_sec", 0) / 1e6
        ov_lap = m_old_lap.get(cfg, {}).get("msgs_per_sec", 0) / 1e6
        diff_ps = pct_diff(nv, ov_ps)
        diff_lap = pct_diff(nv, ov_lap)
        print(f"{cfg:<30} | {nv:<15.2f} | {ov_ps:<15.2f} | {ov_lap:<15.2f} | {diff_ps:>+6.1f}%      | {diff_lap:>+6.1f}%")

def compare_deep_dive():
    old_ps = load_json("benchmarks/results/.backup_prior_to_run/laptop_powersave/deep_dive_features.json")
    old_lap = load_json("benchmarks/results/.backup_prior_to_run/laptop/deep_dive_features.json")
    new_ps = load_json("benchmarks/results/laptop_powersave/deep_dive_features.json")

    print("\n" + "="*95)
    print(" 2. DEEP DIVE ARCHITECTURAL FEATURES COMPARISON")
    print("="*95)
    if not new_ps:
        print("No new deep dive data found.")
        return

    # Adaptive vs Zerospin
    avz_new = new_ps.get("adaptive_vs_zerospin", {})
    avz_old_ps = old_ps.get("adaptive_vs_zerospin", {}) if old_ps else {}
    avz_old_lap = old_lap.get("adaptive_vs_zerospin", {}) if old_lap else {}

    print("\n--- [Adaptive Wait vs Zero-Spin Policy] ---")
    print(f"{'Policy / Metric':<30} | {'New (Bat Perf)':<15} | {'Old (Bat Save)':<15} | {'Old (AC Perf)':<15} | {'vs Bat Save':<12}")
    print("-" * 95)
    for pol in ["zerospin", "adaptive"]:
        pn = avz_new.get(pol, {})
        po_ps = avz_old_ps.get(pol, {})
        po_lap = avz_old_lap.get(pol, {})
        
        thr_n = pn.get("msgs_per_sec", 0) / 1e6
        thr_o_ps = po_ps.get("msgs_per_sec", 0) / 1e6
        thr_o_lap = po_lap.get("msgs_per_sec", 0) / 1e6
        diff_thr = pct_diff(thr_n, thr_o_ps)
        print(f"{pol.capitalize()} Throughput (Mops/s)  | {thr_n:<15.2f} | {thr_o_ps:<15.2f} | {thr_o_lap:<15.2f} | {diff_thr:>+6.1f}%")

        p50_n = pn.get("p50_ns", 0)
        p50_o_ps = po_ps.get("p50_ns", 0)
        p50_o_lap = po_lap.get("p50_ns", 0)
        diff_p50 = pct_diff(p50_n, p50_o_ps)
        print(f"{pol.capitalize()} p50 Latency (ns)     | {p50_n:<15.1f} | {p50_o_ps:<15.1f} | {p50_o_lap:<15.1f} | {diff_p50:>+6.1f}%")

    # PubSub Multicast
    ps_new = {item["subs"]: item for item in new_ps.get("pubsub", [])}
    ps_old_ps = {item["subs"]: item for item in old_ps.get("pubsub", [])} if old_ps else {}
    ps_old_lap = {item["subs"]: item for item in old_lap.get("pubsub", [])} if old_lap else {}

    print("\n--- [Pub/Sub 1-to-N Multicast Throughput (Delivered Mops/s)] ---")
    print(f"{'Subscribers':<30} | {'New (Bat Perf)':<15} | {'Old (Bat Save)':<15} | {'Old (AC Perf)':<15} | {'vs Bat Save':<12}")
    print("-" * 95)
    for s in sorted(ps_new.keys()):
        nv = ps_new[s]["delivered_msgs_s"] / 1e6
        ov_ps = ps_old_ps.get(s, {}).get("delivered_msgs_s", 0) / 1e6
        ov_lap = ps_old_lap.get(s, {}).get("delivered_msgs_s", 0) / 1e6
        diff_ps = pct_diff(nv, ov_ps)
        print(f"{s} Subscriber(s)                | {nv:<15.2f} | {ov_ps:<15.2f} | {ov_lap:<15.2f} | {diff_ps:>+6.1f}%")

    # Reactor
    r_n = new_ps.get("reactor", {})
    r_o_ps = old_ps.get("reactor", {}) if old_ps else {}
    r_o_lap = old_lap.get("reactor", {}) if old_lap else {}
    print("\n--- [Async Eventfd Reactor] ---")
    print(f"{'Metric':<30} | {'New (Bat Perf)':<15} | {'Old (Bat Save)':<15} | {'Old (AC Perf)':<15} | {'vs Bat Save':<12}")
    print("-" * 95)
    r_ev_n = r_n.get("events_per_sec", 0) / 1e3
    r_ev_ps = r_o_ps.get("events_per_sec", 0) / 1e3
    r_ev_lap = r_o_lap.get("events_per_sec", 0) / 1e3
    print(f"{'Events/sec (k events/s)':<30} | {r_ev_n:<15.2f} | {r_ev_ps:<15.2f} | {r_ev_lap:<15.2f} | {pct_diff(r_ev_n, r_ev_ps):>+6.1f}%")
    r_lat_n = r_n.get("avg_latency_ns", 0)
    r_lat_ps = r_o_ps.get("avg_latency_ns", 0)
    r_lat_lap = r_o_lap.get("avg_latency_ns", 0)
    print(f"{'Average Latency (ns)':<30} | {r_lat_n:<15.1f} | {r_lat_ps:<15.1f} | {r_lat_lap:<15.1f} | {pct_diff(r_lat_n, r_lat_ps):>+6.1f}%")

def compare_competitors():
    old_ps = load_json("benchmarks/results/.backup_prior_to_run/laptop_powersave/competitors_benchmark.json")
    old_lap = load_json("benchmarks/results/.backup_prior_to_run/laptop/competitors_benchmark.json")
    new_ps = load_json("benchmarks/results/laptop_powersave/competitors_benchmark.json")

    print("\n" + "="*95)
    print(" 3. COMPETITORS BENCHMARK COMPARISON (librbipc vs DPDK, Disruptor, mqueue, AF_UNIX)")
    print("="*95)
    if not new_ps:
        return

    # Check 1P-1C streaming
    s_new = {item["name"]: item for item in new_ps.get("throughput_64b", [])}
    s_old_ps = {item["name"]: item for item in old_ps.get("throughput_64b", [])} if old_ps else {}
    s_old_lap = {item["name"]: item for item in old_lap.get("throughput_64b", [])} if old_lap else {}

    print("\n--- [1P-1C 64B Streaming Throughput (Mops/s)] ---")
    print(f"{'Queue / IPC Mechanism':<30} | {'New (Bat Perf)':<15} | {'Old (Bat Save)':<15} | {'Old (AC Perf)':<15} | {'vs Bat Save':<12}")
    print("-" * 95)
    for sys_name in sorted(s_new.keys()):
        nv = s_new[sys_name].get("ops_sec", 0) / 1e6
        ov_ps = s_old_ps.get(sys_name, {}).get("ops_sec", 0) / 1e6
        ov_lap = s_old_lap.get(sys_name, {}).get("ops_sec", 0) / 1e6
        diff_ps = pct_diff(nv, ov_ps)
        print(f"{sys_name:<30} | {nv:<15.2f} | {ov_ps:<15.2f} | {ov_lap:<15.2f} | {diff_ps:>+6.1f}%")

    # MPMC Contention in competitors
    m_new = {item["config"]: item for item in new_ps.get("mpmc_scalability", [])}
    m_old_ps = {item["config"]: item for item in old_ps.get("mpmc_scalability", [])} if old_ps else {}
    m_old_lap = {item["config"]: item for item in old_lap.get("mpmc_scalability", [])} if old_lap else {}

    print("\n--- [MPMC Contention Scalability (Mops/s): librbipc vs DPDK vs Disruptor] ---")
    for cfg in ["1P-1C", "2P-2C", "4P-2C", "4P-4C"]:
        item_n = m_new.get(cfg, {})
        item_o_ps = m_old_ps.get(cfg, {})
        item_o_lap = m_old_lap.get(cfg, {})
        print(f"\nConfig: {cfg}")
        print(f"{'System':<25} | {'New (Bat Perf)':<15} | {'Old (Bat Save)':<15} | {'Old (AC Perf)':<15} | {'vs Bat Save':<12}")
        print("-" * 85)
        for sname in ["librbipc", "rte_ring", "disruptor"]:
            nv = item_n.get(sname, 0) / 1e6
            ov_ps = item_o_ps.get(sname, 0) / 1e6
            ov_lap = item_o_lap.get(sname, 0) / 1e6
            diff_ps = pct_diff(nv, ov_ps)
            print(f"{sname:<25} | {nv:<15.2f} | {ov_ps:<15.2f} | {ov_lap:<15.2f} | {diff_ps:>+6.1f}%")

def compare_crc32c():
    old_crc = load_json("benchmarks/results/.backup_prior_to_run/laptop/bench_crc32c.json")
    new_crc = load_json("benchmarks/results/laptop_powersave/bench_crc32c.json")

    print("\n" + "="*95)
    print(" 4. CRC32C HARDWARE ACCELERATION COMPARISON")
    print("="*95)
    if not new_crc:
        return

    n_runs = {item["label"]: item for item in new_crc.get("benchmarks", [])}
    o_runs = {item["label"]: item for item in old_crc.get("benchmarks", [])} if old_crc else {}

    print("\n--- [CRC32C Hardware SSE4.2 Throughput (MB/s)] ---")
    print(f"{'Buffer Geometry':<30} | {'New (Bat Perf)':<15} | {'Old (AC Perf)':<15} | {'Table SW (MB/s)':<15} | {'HW Speedup':<12}")
    print("-" * 95)
    for label in n_runs.keys():
        item = n_runs[label]
        hw_new = item.get("hw_throughput_mb_s", 0)
        sw_new = item.get("table_throughput_mb_s", 0)
        hw_old = o_runs.get(label, {}).get("hw_throughput_mb_s", 0)
        sp = item.get("speedup", 0)
        print(f"{label:<30} | {hw_new:<15.1f} | {hw_old:<15.1f} | {sw_new:<15.1f} | {sp:>5.2f}x")

def main():
    compare_zero_spin()
    compare_deep_dive()
    compare_competitors()
    compare_crc32c()

if __name__ == "__main__":
    main()
