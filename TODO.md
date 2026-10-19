# Handover Document: Post-14 Commits Analysis & Optimal Benchmark Suite

## 1. Executive Summary & Task Overview
This document provides complete, unambiguous context for the next AI agent resuming the task requested under `/goal`:
1. **Deeply analyze the last 14 commits** in `librbipc` to understand all architectural, concurrency, and API changes.
2. **Audit and fix any incongruence or error** at root cause (no workarounds, full structural correctness).
3. **Generate an optimal benchmark suite** (`benchmarks/bench_optimal_suite.c`) tailored for the **Home PC host** (`home_pc` profile) that comprehensively quantifies the performance gains achieved by all 14 recent commits.
4. **Execute the benchmark suite**, route JSON telemetry to `benchmarks/results/home_pc/optimal_benchmark.json`, and synthesize a complete performance report.

---

## 2. Host Environment & System Specification
- **Hardware Profile Name**: `home_pc` (recognized by `benchmarks/bench_common.h`).
- **CPU**: Intel(R) Core(TM) i5-10400F CPU @ 2.90GHz (Comet Lake, 6 physical cores, 12 logical threads; 10 vCPUs assigned to WSL2).
- **Caches**: 32KB L1d/L1i per core, 256KB L2 per core, 12MB shared L3 cache.
- **Vector Instruction Sets**: AVX2, AVX, SSE4.2, SSE4.1, SSSE3, BMI1, BMI2, POPCNT, CLFLUSHOPT. (Does **not** support AVX-512).
- **OS Kernel**: Linux 6.18.33.2-microsoft-standard-WSL2 (Debian distribution).
- **Kernel Features**:
  - `futex2` (`sys_futex_waitv`, syscall 449) is supported by the kernel for 32-bit wait elements; 64-bit futex wait is not enabled in this WSL kernel build.
  - HugeTLB 2MB pages are supported by the kernel architecture, but `nr_hugepages` is currently 0 in this guest instance.
- **Repository Path**: `/home/chrst/repos/librbipc` (Windows UNC: `\\wsl.localhost\Debian\home\chrst\repos\librbipc`).
- **CRITICAL EXECUTION RULE**:
  - Run all shell operations via: `pwsh -NoProfile -Command "wsl -d Debian -- bash -c '<command>'"`
  - The `-NoProfile` flag is mandatory because the host's default PowerShell profile loads `WinGetCommandNotFoundFeedbackPredictor`, which throws an unhandled pipeline exception when commands finish.
  - **NEVER** obfuscate code, and **NEVER** generate code through other code (no base64 wrappers or python code-generating-code scripts). Write and edit files directly and clearly.

---

## 3. Deep Analysis of the Last 14 Commits
The last 14 commits (from `ceed2b0`/`e1869ca` up to HEAD) introduced major architectural optimizations:

1. **`ceed2b0` + `e1869ca`: Anonymous SHM Default and 3-Tier Performance Optimization**
   - **Anonymous Shared Memory Default**: Switched `rbipc_create(NULL, ...)` to use `memfd_create()`, avoiding all `/dev/shm` filesystem path lookups, dentry creation, permission checking, and unlinking. Descriptors are passed across processes via UNIX domain sockets (`SCM_RIGHTS`).
   - **Tier 1 (Zero-Trust, Default)**: Robust multi-process safety with bounded FAA reservation (Nikolaev SCQ pattern), atomic state machine (`RESERVING` -> `RESERVED` -> `COMMITTED`), acquire timestamps, and producer PID tracking.
   - **Tier 2 (Trusted Mode, `RBIPC_FLAG_TRUSTED`)**: Elides intermediate state machines and PID tracking. Uses pure turn-based Vyukov sequence tracking. Cuts memory writes per message from 3 cache line writes down to 1.
   - **Tier 3 (Packed Mode, `RBIPC_FLAG_PACKED`)**: Bit-packs sequence (48 bits), payload length (12 bits), and slot state (4 bits) into a single 64-bit atomic integer (`sequence`), turning commit and release into single 64-bit atomic stores (`rbipc_slot_commit_packed`, `rbipc_slot_release_packed`).

2. **`547b575`: feat(simd): implement AVX1 fallback scanner for legacy 256-bit architectures without AVX2**
   - Implemented `rbipc_simd_scan_avx` in `src/io/rbipc_simd.c`. Allows processors with 256-bit AVX floating-point registers but no AVX2 integer instructions (Sandy Bridge, Ivy Bridge) to scan 4 slot sequences per instruction.

3. **`e5229b1`: docs: document 12-bit packed payload trade-off, zero-copy bandwidth physics, and AVX fallback**
   - Documented the architectural trade-off of Tier 3 (12-bit length = 4096-byte slot max), physical DRAM GB/s saturation vs virtual cache throughput, and vector fallback tiers.

4. **`f15eb14`: fix(pubsub): support Tier 3 packed rings in 1-to-N multicast pubsub**
   - Updated multicast subscriber lag computation and batch polling (`rbipc_sub_poll`, `rbipc_sub_poll_batch`) to unpack sequences and lengths from 64-bit packed words.

5. **`cab5c69`: fix(audit): support Tier 3 packed rings in crash recovery, lease revocation, and hole healing**
   - Updated ticket hole healing (`rbipc_audit_check_and_heal_ticket_hole`) and lease revocation (`rbipc_audit_revoke_expired_leases`) to unpack states and sequences from packed slots.

6. **`816484b`: fix(telemetry): unpack slot state from packed descriptors in rbipc_get_slot_stats**
   - Fixed telemetry reporting in `src/core/rbipc_ring.c` so monitoring tools accurately report slot state in packed rings.

7. **`a5d6d4e`: docs(readme): refresh symbol count, test suite matrix, and code coverage metrics**
   - Synchronized documentation with public API symbols and test coverage.

8. **`a36e9d0`: feat(pubsub): implement zero-copy multicast publishing and abort handling**
   - Implemented `rbipc_publish_reserve`, `rbipc_publish_commit`, and `rbipc_publish_abort` in `src/ipc/rbipc_pubsub.c`. Publishers reserve slot space directly, write without intermediate buffers, and commit across N subscribers.

9. **`bad69a3`: feat(ipc): implement multi-FD transmission and auto-binding eventfd reactor**
   - Implemented `rbipc_ipc_send_fds` and `rbipc_ipc_recv_fds` in `src/ipc/rbipc_ipc.c` to transmit up to 8 file descriptors in a single `sendmsg` call. Also enabled automatic eventfd binding on socket reception.

10. **`009664a`: feat(shm): support HugeTLB 2MB pages for anonymous shared memory**
    - Added `RBIPC_FLAG_HUGETLB` and `rbipc_shm_hugetlb_available()` in `src/memory/rbipc_shm.c` using `MFD_HUGETLB | MFD_HUGE_2MB`, reducing TLB misses by 512x for large ring buffers.

11. **`91635bc`: perf(sync): memoize futex2 64-bit probe capability**
    - Memoized `rbipc_futex2_has_u64()` in `src/sync/rbipc_futex.c` with atomic caching to eliminate redundant kernel syscall probes.

12. **`a3e0376`: docs: update public API symbol count to 79 in README**

13. **`c26143d`: fix(shm): enforce unsigned literals for HugeTLB macros to satisfy clang-tidy**

14. **`35a46fd`: chore: add eventual AI's scratchpad folder**

---

## 4. Current State of the Codebase & Completed Fixes
Four critical bugs were identified, repaired at the root cause, and verified with `make clean && make -j8 && make test`:

1. **`include/rbipc.h`**:
   - Fixed missing `#` before `include <assert.h>` on line 43. Cleanly includes `<assert.h>` and `<stdalign.h>`.
2. **`src/audit/rbipc_audit.c` (`rbipc_audit_recover_dead_producer`)**:
   - **Root Cause**: The function did not check whether `turn == ticket`. When slots in Tier 3 progressed past `ticket`, residual reserved state or uninitialized state caused readers to execute expensive `kill(pid, 0)` syscalls repeatedly in the spin backoff path.
   - **Fix Applied**: Added sequence turn validation (`uint64_t turn = is_packed ? rbipc_slot_unpack_seq(raw_seq) : raw_seq; if (turn != ticket) return false;`) and unpacked `state` from `raw_seq` for packed rings while checking `slot->state` for in-flight reservations.
3. **`src/core/rbipc_slot.c`**:
   - **Fix Applied**: In `rbipc_slot_commit_packed`, set `slot->state = RBIPC_SLOT_COMMITTED`. In `rbipc_slot_release_packed`, set `slot->state = RBIPC_SLOT_EMPTY` and cleared `producer_pid = 0`, `consumer_pid = 0`, and `acquire_timestamp_ns = 0` to preserve metadata consistency for zero-trust fallback and audit.
4. **`tests/io/test_e2e_throughput.c`**:
   - **Root Cause**: Child processes called standard `exit(0)`, causing duplicate stdout buffer flushes when standard output was redirected or piped.
   - **Fix Applied**: Added `fflush(stdout)` immediately before `fork()` and used `_exit(0)` in child processes.

All 26 test binaries currently pass 100% without warnings under `-std=c23 -Wall -Wextra -Wpedantic -Werror`.

---

## 5. Blueprint for the Optimal Benchmark Suite (`benchmarks/bench_optimal_suite.c`)
The optimal benchmark suite must evaluate the concrete gains of the 14 commits across 7 dedicated modules:

### Module 1: 3-Tier Latency Hierarchy (Half-RTT Ping-Pong)
- **Setup**: Pinned producer (CPU 2) and consumer (CPU 3) ping-ponging 64-byte messages across two ring buffers (`SHM_LAT_A` and `SHM_LAT_B`).
- **Iterations**: 50,000 rounds (plus 500 warmup rounds).
- **Modes**:
  1. Tier 1 Zero-Trust (`flags = 0`)
  2. Tier 2 Trusted (`flags = RBIPC_FLAG_TRUSTED`)
  3. Tier 3 Packed (`flags = RBIPC_FLAG_PACKED`)
  4. Tier 2+3 Combined (`flags = RBIPC_FLAG_TRUSTED | RBIPC_FLAG_PACKED`)
- **Metrics**: Min, P25, P50, P75, P90, P99, P99.9, Max, Mean (in ns, half-RTT).
- **Expected Outcome**: Minimum half-RTT drops from ~100 ns (Tier 1) to ~50 ns (Tier 2/3), achieving a ~50% latency reduction.

### Module 2: Streaming 1P-1C Throughput & Cache Saturation
- **Setup**: Unidirectional streaming of 200,000 messages (64-byte payload).
- **Execution**: Evaluated both **Pinned** (CPU 2 -> CPU 3) and **Unpinned** across all 4 modes.
- **Metrics**: Throughput (msgs/sec), Bandwidth (MB/sec), Latency per message (ns/msg).
- **Expected Outcome**: Pinned streaming reaches 14.8M - 16.5M msgs/sec (~60-67 ns/msg).

### Module 3: B-Queue Vector Batching Scaling
- **Setup**: Batching via `rbipc_write_batch` and `rbipc_read_acquire_batch`.
- **Batch Sizes**: 1, 4, 16, 32, 64, 128 (500,000 messages total per batch size).
- **Modes**: Tier 1 Zero-Trust vs Combined Tier 2+3.
- **Metrics**: Throughput (msgs/sec), Bandwidth (MB/sec), Latency (ns/msg), and Speedup factor vs batch size 1.
- **Expected Outcome**: Demonstrates scaling from ~9M msgs/sec (batch 1) to >45M msgs/sec (>6.2 GB/s, ~21 ns/msg) at batch 32+.

### Module 4: Anonymous Shared Memory vs Named POSIX Shm Lifecycle
- **Setup**: 2,000 cycles of create, attach, detach, and destroy.
- **Modes**:
  - Anonymous: `rbipc_create(NULL, ...)` (`memfd_create`) + SCM_RIGHTS FD transfer (`rbipc_send_fd`/`rbipc_recv_fd`).
  - Named: `rbipc_create("/shm_name", ...)` + `rbipc_attach("/shm_name", ...)` + `rbipc_destroy()`.
- **Metrics**: Operations/sec, average latency per lifecycle cycle in microseconds (μs), and speedup factor.
- **Expected Outcome**: Anonymous shared memory eliminates filesystem lock contention and directory inode creation in `/dev/shm`.

### Module 5: Multicast PubSub Zero-Copy vs Standard Copy
- **Setup**: 50,000 messages published to 1-to-N subscribers across fanouts: 1, 2, 4, 8.
- **Modes**:
  1. Standard Copy Multicast (`rbipc_publish`) on Standard Ring.
  2. Standard Copy Multicast (`rbipc_publish`) on Tier 3 Packed Ring.
  3. Zero-Copy Multicast (`rbipc_publish_reserve`/`commit`) on Standard Ring.
  4. Zero-Copy Multicast (`rbipc_publish_reserve`/`commit`) on Tier 3 Packed Ring.
- **Metrics**: Publisher msgs/sec, Effective delivery msgs/sec (`fanout * pub_msgs_sec`), Effective delivery bandwidth (MB/sec).
- **Expected Outcome**: Zero-copy publishing eliminates N duplicate memcpys and scales linearly with subscriber fanout without saturating physical memory buses.

### Module 6: SIMD Sequence Scanning Engines
- **Setup**: 2,500,000 scanning iterations (evaluating 10,000,000 slots total) on a 256-slot sequence array.
- **Engines**:
  - `rbipc_simd_scan_scalar`
  - `rbipc_simd_scan_sse41`
  - `rbipc_simd_scan_avx` (AVX1 fallback, commit `547b575`)
  - `rbipc_simd_scan_avx2`
- **Evaluation**: Run on both Standard 64-bit sequences and Tier 3 packed sequences (16-bit shift).
- **Metrics**: Million slots scanned / second (Mslots/sec), Speedup vs scalar.
- **Expected Outcome**: AVX2 achieves 4 slots/cycle (~3-4x faster than scalar); AVX1 fallback provides ~2.5x speedup over scalar without AVX2.

### Module 7: Multi-FD Transmission, Futex2 Memoization, & HugeTLB
- **Multi-FD Transmission**: Transfer 1 FD, 2 FDs, 4 FDs in a single `rbipc_ipc_send_fds` call vs 4 sequential socket calls (5,000 iterations).
- **Futex2 Memoization**: 500,000 checks of `rbipc_futex2_has_u64()` vs raw `syscall(__NR_futex_waitv, NULL, 0, 0, 0)`.
- **HugeTLB 2MB**: Checks `rbipc_shm_hugetlb_available()` and documents capability status.

### Module 8: JSON Telemetry & Console Reporting
- Integrates with `benchmarks/bench_common.h` using `bench_get_result_path(path, sizeof(path), "optimal_benchmark.json")`.
- Writes JSON telemetry to `benchmarks/results/home_pc/optimal_benchmark.json`.
- Prints human-readable summary tables to stdout.

---

## 6. Actionable Next Steps for Resuming Agent
1. **Verify Workspace State**:
   Run `pwsh -NoProfile -Command "wsl -d Debian -- bash -c 'cd /home/chrst/repos/librbipc && git status && make -j8 && make test'"` to confirm everything builds and passes cleanly.
2. **Implement Benchmark Suite**:
   Write `benchmarks/bench_optimal_suite.c` following the blueprint in Section 5.
   - Note: In `Makefile`, `BENCH_SRCS = $(wildcard $(BENCHDIR)/*.c)` automatically includes `benchmarks/bench_optimal_suite.c` and builds `bin/bench_optimal_suite`.
3. **Build the Benchmark**:
   Run `pwsh -NoProfile -Command "wsl -d Debian -- bash -c 'cd /home/chrst/repos/librbipc && make bin/bench_optimal_suite'"`
4. **Execute and Profile**:
   Run `pwsh -NoProfile -Command "wsl -d Debian -- bash -c 'cd /home/chrst/repos/librbipc && RBIPC_BENCH_PROFILE=home_pc bin/bench_optimal_suite'"`
5. **Inspect Generated Telemetry**:
   Verify `benchmarks/results/home_pc/optimal_benchmark.json` was generated.
6. **Compile Comprehensive Analysis & Final Report**:
   Synthesize all performance gains, include tables and markdown links, and conclude the `/goal` task with `<!-- GOAL_COMPLETE -->`.
