# `librbipc`: Lock-Free Shared-Memory Ring Buffer IPC

[![Standard: ISO C23](https://img.shields.io/badge/C-C23-blue.svg)](https://en.cppreference.com/w/c/23)
[![Platform: Linux](https://img.shields.io/badge/Platform-Linux-FCC624.svg)](https://www.kernel.org/)
[![Coverage: 82.65%](https://img.shields.io/badge/Coverage-82.65%25-brightgreen.svg)](#test-coverage)
[![License: Apache 2.0](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](https://opensource.org/licenses/Apache-2.0)

`librbipc` is a zero-dependency, production-grade Inter-Process Communication (IPC) library written in ISO C23 for Linux. It implements a lock-free Multi-Producer / Multi-Consumer (MPMC) shared-memory ring buffer engineered for **sub-microsecond user-space fast paths**, **zero idle CPU consumption (0.13%)** via Dekker-synchronized Linux `sys_futex` suspension, **deterministic crash recovery** (`pidfd_open(2)`), **B-Queue scatter-gather vector batching** (**50.68M msgs/s**), **non-temporal cache-bypassing streaming stores** (**19.87 GB/s**), **asynchronous `eventfd`/`epoll` reactor signaling**, and **1-to-N lock-free multicast publish-subscribe**.

For the complete formal concurrency model, memory layout geometry, crash-recovery state machines, and empirical comparisons against **DPDK `rte_ring`**, **LMAX Disruptor**, **Eclipse `iceoryx2`**, **POSIX `mqueue`**, and **`AF_UNIX`**, see **[`TECHNICAL_REPORT.md`](TECHNICAL_REPORT.md)**.

---

## Core Architectural Features

- **Lock-Free Vyukov 64-Bit Sequence Turn Engine:** Eliminates global `head`/`tail` CAS spin-wait bottlenecks using 64-bit monotonic `atomic_fetch_add` ticket allocation paired with per-slot 64-bit sequence turn verification (`slot->sequence == ticket` for vacancy, `slot->sequence == ticket + 1` for readiness), decoupled from 32-bit kernel futex words to prevent ABA wrap-around aliasing.
- **Unified 4-Tier Hybrid Synchronization Engine:** Under sustained load, 100% of operations execute in user space with **zero system calls** and **zero hardware memory fences** on the commit/release fast path:
  - **Tier 0 (Micro-Turnaround):** Bounded user-space micro-spin (`RBIPC_WAIT_ADAPTIVE`, 48 `pause`/`yield` cycles ≈ 400 ns) catching in-flight commits.
  - **Tier 1 (Hardware-Assisted Wait):** User-level monitor-wait using **Intel WAITPKG** (`UMONITOR` + `UMWAIT` in low-latency C0.1 sub-state, `TPAUSE`) and **ARM64 FEAT_WFxT** (`WFET` + exclusive monitors), providing 40–70 ns hardware wakeup with minimal core power consumption.
  - **Tier 2 (Asymmetric Dekker & Linux 5.16+ `futex2`):** Eliminates CPU store buffer stalls (`MFENCE`) on high-frequency fast paths via **Asymmetric Dekker Synchronization** (`sys_membarrier(2)` on waiter registration). Implements native **Linux 5.16+ `futex2` (`sys_futex_waitv`)** vectorized multi-wait, monitoring sequence words and cooperative shutdown flags simultaneously with zero heartbeat polling drift.
  - **Tier 3 (Deep Suspension & Fault Healing):** Deterministic passive `SYS_futex` sleep with dual-word atomic `FUTEX_WAKE_OP` operations.
- **Race-Free Crash Resilience & Time-Bounded Consumer Leases:** Uses Linux 5.3+ `pidfd_open(2)` + `poll(2)` (with `kill(pid, 0)` fallback) to audit slot-holding peers without PID-recycling races or zombie blind spots. Automatically poisons slots abandoned by crashed producers (`RBIPC_SLOT_POISONED`), heals orphaned ticket holes after a 50 ms grace period, recycles slots held by dead consumers, and enforces optional nanosecond consumer leases (`rbipc_set_lease_timeout`, `rbipc_revoke_expired_leases`).
- **Hardware Integrity & `SIGBUS` Protection:** Seals shared memory descriptors via `fcntl(F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW)` and validates the 256-byte 4-cache-line control header (`rbipc_shm_header_t`) on attachment using hardware SSE4.2 CRC32C (`_mm_crc32_u64`) and a two-phase `RBIPC_HEADER_INIT_DONE` publication sentinel.
- **B-Queue Vector Batching & SIMD Scanning:** Amortizes atomic ticket allocation and memory fences across up to `capacity` contiguous slots (`rbipc_reserve_write_batch*`, `rbipc_read_acquire_batch*`), accelerated by 256-bit x86-64 AVX2 (`_mm256_cmpeq_epi64`) and 128-bit ARM64 NEON (`vceqq_u64`) sequence scanners.
- **Non-Temporal Streaming Stores:** Transfers payloads ≥ 256 B directly through Write-Combining buffers via `_mm256_stream_si256` / `_mm_stream_si128` + `_mm_sfence()` (`rbipc_stream_copy`, `rbipc_write_stream*`), preventing L1/L2/L3 cache pollution and Read-For-Ownership (RFO) bus traffic.
- **Multi-Topology IPC Extensions:**
  - **Reactor Integration:** Non-blocking `eventfd(2)` readiness notifications (`rbipc_enable_eventfd`, `rbipc_eventfd_drain`) for `epoll(7)` and `io_uring` event loops.
  - **1-to-N Multicast Pub-Sub:** Wait-free single/batch topic publishing (`rbipc_publish`, `rbipc_publish_batch`) with independent subscriber cursors (`rbipc_sub_t`) and automatic lag clamping (`rbipc_sub_poll*`, `rbipc_sub_read`).
  - **Capability FD Passing:** Zero-namespace `memfd_create` descriptor transmission over `AF_UNIX` `SCM_RIGHTS` (`rbipc_send_fd`, `rbipc_recv_fd_ex`).

---

## Repository Structure & Module Decomposition

```text
librbipc/
├── include/
│   ├── rbipc.h              # Public C23/C++ ABI: core ring, zero-copy I/O, convenience copy I/O, batching, streaming, reactor, leases
│   ├── rbipc_ipc.h          # Public ABI: UNIX domain socket SCM_RIGHTS descriptor passing
│   └── rbipc_pubsub.h       # Public ABI: 1-to-N lock-free multicast publish-subscribe engine
├── src/
│   ├── rbipc_arch.h         # Monotonic clock (vDSO), CPU pause/yield, SSE4.2 + SW CRC32C (Castagnoli)
│   ├── rbipc_attr.h         # Compiler inlining (RBIPC_INLINE) and cache prefetch hints (RBIPC_PREFETCH)
│   ├── rbipc_math.h         # Power-of-two rounding, alignment, and 64-bit RFC 1982 modular sequence delta
│   ├── rbipc_predicate.h    # Pure, side-effect-free invariant validation predicates
│   ├── rbipc_internal.h     # Private runtime handle (struct rbipc_ring) and internal constants
│   ├── rbipc_error.c/.h     # Status code descriptions and Linux errno translation
│   ├── rbipc_shm.c/.h       # Overflow-safe layout geometry, shm_open / memfd_create, F_ADD_SEALS
│   ├── rbipc_vmem.c/.h      # Control mapping, 1x linear slot mapping, and 2x MAP_FIXED virtual ring mirror
│   ├── rbipc_futex.c/.h     # Process-shared Linux sys_futex (FUTEX_WAIT / FUTEX_WAKE) wrappers
│   ├── rbipc_sync.c/.h      # Dekker seq_cst waiter registration and zero-spin / adaptive backoff engine
│   ├── rbipc_slot.c/.h      # Per-slot Vyukov sequence state machine and CAS-guarded consumer release
│   ├── rbipc_audit.c/.h     # pidfd_open(2) peer liveness auditing, slot poisoning, ticket-hole & lease recovery
│   ├── rbipc_simd.c/.h      # AVX2 (256-bit), NEON (128-bit), and scalar sequence vacancy/readiness scanners
│   ├── rbipc_stream.c/.h    # Non-temporal cache-bypassing memory streaming (_mm256_stream_si256)
│   ├── rbipc_reactor.c/.h   # Asynchronous eventfd(2) lifecycle, signaling, and counter draining
│   ├── rbipc_ring.c/.h      # Ring creation/attachment lifecycle, CRC32C verification, and telemetry
│   ├── rbipc_io.c/.h        # Zero-copy single-slot reservation, commit, abort, acquisition, and release
│   ├── rbipc_batch.c/.h     # B-Queue vector batch scatter-gather reservation and acquisition
│   ├── rbipc_ipc.c          # Ancillary SCM_RIGHTS file descriptor transmission and attachment
│   └── rbipc_pubsub.c       # 1-to-N multicast topic publishing and independent subscriber cursor polling
├── tests/                   # 25 automated verification suites (16 unit, 3 I/O, 6 E2E & chaos fault-injection)
├── benchmarks/              # 5 benchmark suites + partitioned datasets (benchmarks/results/{home_pc,laptop,laptop_powersave}/*.json)
│   ├── bench_common.h       # Shared hardware telemetry, path routing, and adaptive spin backoff
│   ├── bench_suite.c        # Core latency, 1P-1C streaming, B-Queue batching, zero-copy payloads, MPMC
│   ├── bench_deep_dive.c    # Adaptive spin, non-temporal streaming stores, pub-sub, reactor, SIMD, leases
│   ├── bench_competitors.c  # Cross-system: librbipc vs. DPDK rte_ring, LMAX Disruptor, mqueue, AF_UNIX
│   ├── bench_idle_strategies.c # 10-strategy evaluation across DPDK, LMAX Disruptor, and librbipc wait policies
│   └── bench_librbipc_isolated.c # Isolated microbenchmarks (pointer exchange, payload copies, B-Queue batches)
├── scripts/
│   ├── coverage_summary.py  # Automated gcov line coverage parser and tabular reporter
│   ├── doxygen_md_filter.py # Doxygen Markdown input filter for clean MathJax LaTeX parsing
│   ├── gen_compile_commands.py # Clang-Tidy compilation database generator
│   └── postprocess_docs.py  # Doxygen HTML asset bundler and navigation post-processor
├── TECHNICAL_REPORT.md      # Unified architectural monograph, formal proofs, and empirical evaluation
├── TODO.md                  # Next-generation synchronization hierarchy specification and roadmap
├── Doxyfile                 # Doxygen API and architectural documentation configuration
└── Makefile                 # Build, test, sanitizer, static analysis, coverage, and documentation targets
```

---

## Public API Overview

### 1. Lifecycle, Attachment, & Wait Policies (`include/rbipc.h`)

| Function | Description |
| :--- | :--- |
| `rbipc_create` / `rbipc_create_ex` | Create and initialize a named (`shm_open`) or anonymous (`memfd_create`) shared-memory ring buffer with explicit role flags (`RBIPC_ROLE_PRODUCER`, `RBIPC_ROLE_CONSUMER`, `RBIPC_ROLE_MONITOR`) and permissions. |
| `rbipc_attach` / `rbipc_attach_ex` | Attach to an existing named shared-memory ring buffer and verify header magic, endianness, CRC32C, and layout geometry. |
| `rbipc_attach_fd` / `rbipc_attach_fd_ex` | Attach directly to an open shared-memory file descriptor (e.g., received via `SCM_RIGHTS`). |
| `rbipc_detach` / `rbipc_destroy` | Unmap virtual memory regions, decrement active role counters, close descriptors, or unlink a named POSIX shared memory segment. |
| `rbipc_signal_shutdown` / `rbipc_shutdown` | Set the atomic shutdown flag and broadcast a wakeup (`FUTEX_WAKE_ALL`) to all blocked producers and consumers. |
| `rbipc_set_wait_policy` / `rbipc_get_wait_policy` | Configure `RBIPC_WAIT_ZERO_SPIN` (default passive `FUTEX_WAIT`), `RBIPC_WAIT_ADAPTIVE` (bounded micro-spin pause budget before `FUTEX_WAIT`), or `RBIPC_WAIT_HW_MONITOR` (WAITPKG `UMWAIT` C0.2 / ARM64 `WFET` clock-gated hardware wait). |
| `rbipc_get_fd` / `rbipc_get_stats` | Retrieve underlying shared-memory descriptor or query live telemetry statistics (tickets, capacity, active roles). |

### 2. Zero-Copy Single-Slot & Convenience Copy I/O (`include/rbipc.h`)

| Function | Description |
| :--- | :--- |
| `rbipc_reserve_write` / `_nonblock` / `_timeout` | Claim a write ticket and obtain a direct zero-copy pointer (`void **out_buf`) to the cache-aligned slot buffer. |
| `rbipc_commit_write` / `rbipc_abort_write` | Publish a populated slot (`sequence = ticket + 1`) and wake waiting consumers, or poison an aborted reservation (`RBIPC_SLOT_POISONED`). |
| `rbipc_read_acquire` / `_nonblock` / `_timeout` | Claim a read ticket and obtain a direct zero-copy pointer (`const void **out_buf`), payload length, and sequence ticket. |
| `rbipc_read_release` | Recycle a consumed slot (`sequence = ticket + capacity`) and wake waiting producers if space-blocked. |
| `rbipc_write` / `_nonblock` / `_timeout` | Convenience wrapper: reserve → `memcpy` → commit. |
| `rbipc_read` / `_nonblock` / `_timeout` | Convenience wrapper: acquire → `memcpy` → release. |

### 3. B-Queue Vector Batching & Non-Temporal Streaming (`include/rbipc.h`)

| Function | Description |
| :--- | :--- |
| `rbipc_reserve_write_batch` / `_nonblock` / `_timeout` | Reserve up to `count` contiguous slots in a single atomic step, populating an array of `rbipc_iovec_t` zero-copy descriptors. |
| `rbipc_commit_write_batch` | Atomically commit a batch of reserved slots with a single memory fence and batched waiter notification. |
| `rbipc_read_acquire_batch` / `_nonblock` / `_timeout` | Acquire up to `count` ready slots in a single atomic step, populating an array of `rbipc_rovec_t` descriptors. |
| `rbipc_read_release_batch` | Recycle a batch of consumed read slots in a single pass. |
| `rbipc_stream_copy` | Copy memory using AVX2 (`_mm256_stream_si256`) / SSE2 (`_mm_stream_si128`) non-temporal stores + `_mm_sfence()`. |
| `rbipc_write_stream` / `_nonblock` / `_timeout` | Reserve a slot, transfer payload via cache-bypassing non-temporal streaming stores, and commit. |

### 4. Reactor, Leases, Capability IPC, & Multicast Pub-Sub (`rbipc.h`, `rbipc_ipc.h`, `rbipc_pubsub.h`)

| Function | Description |
| :--- | :--- |
| `rbipc_enable_eventfd` / `rbipc_set_eventfd` / `rbipc_get_eventfd` / `rbipc_eventfd_drain` | Manage `eventfd(2)` readiness notifications for `epoll(7)` / `io_uring` event loop integration. |
| `rbipc_set_lease_timeout` / `rbipc_get_lease_timeout` / `rbipc_revoke_expired_leases` | Configure nanosecond consumer slot leases and proactively revoke/recycle slots held by stalled consumers. |
| `rbipc_send_fd` / `rbipc_recv_fd` / `rbipc_recv_fd_ex` | Transmit and attach ring buffer file descriptors across `AF_UNIX` sockets via `SCM_RIGHTS`. |
| `rbipc_publish` / `rbipc_publish_batch` | Publish single or batched topic messages in 1-to-N multicast mode with automatic non-temporal streaming for payloads ≥ 256 B. |
| `rbipc_sub_create` / `rbipc_sub_create_ex` / `rbipc_sub_destroy` | Create or destroy an independent subscriber handle (`rbipc_sub_t`) starting at the latest head or oldest retained slot. |
| `rbipc_sub_poll` / `rbipc_sub_poll_batch` / `rbipc_sub_read` / `rbipc_sub_get_lag` | Non-destructively poll or block on published topic messages with automatic cursor lag clamping. |

---

## Build, Verification, & Documentation

```bash
# Build static (lib/librbipc.a), shared (lib/librbipc.so), and all 25 test binaries (-O3 -std=c23)
make -j4

# Execute the full 25-suite verification matrix (Unit, I/O, MPMC, Crash Recovery, Fault Injection)
make test

# Run AddressSanitizer + UndefinedBehaviorSanitizer or ThreadSanitizer verification
make asan
make tsan

# Run Clang-Tidy static analysis across all source, test, and benchmark units
make clang-tidy

# Generate gcov line coverage report across all 16 src/ modules
make coverage

# Build Doxygen HTML documentation (output in docs/html/index.html)
make docs
```

---

## Test Coverage {#test-coverage}

Verified across all 25 test suites via `make coverage` (`gcov` with `-O0 -g --coverage`):

| Source File | Executable Lines | Covered Lines | Coverage % |
| :--- | ---: | ---: | ---: |
| [`src/rbipc_error.c`](src/rbipc_error.c) | 56 | 56 | **100.00%** |
| [`src/rbipc_io.h`](src/rbipc_io.h) | 13 | 13 | **100.00%** |
| [`src/rbipc_stream.h`](src/rbipc_stream.h) | 4 | 4 | **100.00%** |
| [`src/rbipc_simd.c`](src/rbipc_simd.c) | 56 | 55 | **98.21%** |
| [`src/rbipc_predicate.h`](src/rbipc_predicate.h) | 39 | 38 | 97.44% |
| [`src/rbipc_math.h`](src/rbipc_math.h) | 13 | 12 | 92.31% |
| [`src/rbipc_audit.c`](src/rbipc_audit.c) | 120 | 110 | 91.67% |
| [`src/rbipc_batch.c`](src/rbipc_batch.c) | 181 | 165 | 91.16% |
| [`src/rbipc_slot.c`](src/rbipc_slot.c) | 91 | 82 | 90.11% |
| [`src/rbipc_ring.c`](src/rbipc_ring.c) | 286 | 252 | 88.11% |
| [`src/rbipc_vmem.c`](src/rbipc_vmem.c) | 53 | 46 | 86.79% |
| [`src/rbipc_ipc.c`](src/rbipc_ipc.c) | 73 | 63 | 86.30% |
| [`src/rbipc_pubsub.c`](src/rbipc_pubsub.c) | 149 | 119 | 79.87% |
| [`src/rbipc_shm.c`](src/rbipc_shm.c) | 130 | 102 | 78.46% |
| [`src/rbipc_io.c`](src/rbipc_io.c) | 282 | 219 | 77.66% |
| [`src/rbipc_futex.c`](src/rbipc_futex.c) | 181 | 140 | 77.35% |
| [`src/rbipc_stream.c`](src/rbipc_stream.c) | 54 | 40 | 74.07% |
| [`src/rbipc_internal.h`](src/rbipc_internal.h) | 10 | 7 | 70.00% |
| [`src/rbipc_reactor.c`](src/rbipc_reactor.c) | 70 | 49 | 70.00% |
| [`src/rbipc_sync.c`](src/rbipc_sync.c) | 182 | 126 | 69.23% |
| [`src/rbipc_arch.h`](src/rbipc_arch.h) | 44 | 27 | 61.36% |
| **TOTAL LINE COVERAGE** | **2,087** | **1,725** | **82.65%** |

---

## Performance Benchmarks Summary

Measured on **Intel Core i5-10400F @ 2.90 GHz** (6C/12T desktop) and **Intel Core i5-6300U @ 2.40 GHz** (2C/4T mobile laptop in plugged-in and battery power-save modes; raw JSON datasets in `benchmarks/results/{home_pc,laptop,laptop_powersave}/`):

| Benchmark Workload | Measured Performance | Key Architectural Takeaway |
| :--- | :--- | :--- |
| **Idle CPU Utilization** (2 s quiet window) | **0.13% CPU** (0 involuntary CS) | True kernel `FUTEX_WAIT` suspension vs. 100% core burn in DPDK `rte_ring` / LMAX `BusySpin`. |
| **B-Queue Vector Batch-128** (64 B payload) | **50,676,773 msgs/s** (3,093.1 MB/s, 19.73 ns/msg) | 6.53× speedup over single-slot mode by amortizing ticket CAS, AVX2 scan, and Dekker fence. |
| **B-Queue Vector Batch-32** (64 B payload) | **44,111,267 msgs/s** (2,692.3 MB/s, 22.67 ns/msg) | Outperforms single-slot busy-spin DPDK `rte_ring` (17.81 Mops/s) by 2.48× while using 0.13% idle CPU. |
| **1P-1C Single-Slot Streaming** (64 B payload) | **6.08M – 12.42M msgs/s** (371–758 MB/s) | 23.7× faster than POSIX `mqueue` (395.8K ops/s) and 6.9× faster than `AF_UNIX` (1.36M ops/s). |
| **Half-RTT Ping-Pong Latency** (64 B payload) | **200 ns min** / 25.95 μs p50 (`ADAPTIVE`) | User-space fast path resolves in 200 ns; `RBIPC_WAIT_ADAPTIVE` cuts p99 latency by -12.70%. |
| **Non-Temporal Streaming Copy** (`rbipc_write_stream`) | **19,867.2 MB/s** (64 KiB) / **14,746.3 MB/s** (1 MiB) | +104.97% bandwidth speedup over standard `memcpy` (7,194.2 MB/s at 1 MiB) via AVX2 `_mm256_stream_si256`. |
| **Zero-Copy In-Place Slot Transfer** (1 MiB / 16 MiB) | **5.87M msgs/s** (5.74 TB/s) / **9.86M msgs/s** (154.1 TB/s) | Constant O(1) sequence turn publication overhead independent of slot payload size. |
| **Symmetric `4P-4C` MPMC Contention** (64 B) | **7,019,693 ops/s** (`bench_competitors.c`) | Achieves 87.9% of busy-spin DPDK `rte_ring` (7.98M) and 87.1% of LMAX Disruptor (8.06M). |
| **`eventfd` + `epoll_wait` Reactor** | **874,792 events/s** (1.14 μs/event) | Direct integration with asynchronous `epoll(7)` and `io_uring` event loops. |

See **[`TECHNICAL_REPORT.md`](TECHNICAL_REPORT.md)** for the complete experimental tables, latency distributions, and cross-system analyses.

---

## License

Copyright © 2026 Christian Santarelli (`Seintian`). Licensed under the [Apache License, Version 2.0](https://www.apache.org/licenses/LICENSE-2.0).
