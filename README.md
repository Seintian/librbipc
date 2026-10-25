# `librbipc`: Lock-Free Shared-Memory Ring Buffer IPC

[![Standard: ISO C23](https://img.shields.io/badge/C-C23-blue.svg)](https://en.cppreference.com/w/c/23)
[![Platform: Linux](https://img.shields.io/badge/Platform-Linux-FCC624.svg)](https://www.kernel.org/)
[![License: Apache 2.0](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](https://opensource.org/licenses/Apache-2.0)

`librbipc` is a zero-dependency, production-grade Inter-Process Communication (IPC) library written in ISO C23 for Linux. It implements a lock-free Multi-Producer / Multi-Consumer (MPMC) shared-memory ring buffer engineered for **sub-microsecond user-space fast paths**, **green idle CPU efficiency (0.3% CPU)** via Dekker-synchronized Linux `sys_futex_waitv` suspension, **deterministic crash recovery** (`pidfd_open(2)`), **B-Queue scatter-gather vector batching** (**54.59M msgs/s**), **non-temporal cache-bypassing streaming stores** (**19.87 GB/s**), **asynchronous `eventfd`/`epoll` reactor signaling**, and **1-to-N lock-free multicast publish-subscribe**.

For the detailed architectural specification, memory layout geometry, execution modes, and synchronization engine, see **[`ARCHITECTURE.md`](ARCHITECTURE.md)**. For the empirical benchmark profiling analysis, apples-to-apples comparisons against **DPDK `rte_ring`**, **LMAX Disruptor**, **POSIX `mqueue`**, and **`AF_UNIX`**, low-level `perf` hardware counters, and cache contention analysis, see **[`BENCHMARKS.md`](BENCHMARKS.md)**.

---

## Core Architectural Highlights

- **Lock-Free Vyukov 64-Bit Sequence Turn Engine**: Eliminates global `head`/`tail` CAS bottlenecks using 64-bit monotonic ticket allocation paired with per-slot sequence turn verification. Decouples producer ticket claims from slot writes, eliminating live-lock under contention.
- **Unified 4-Tier Hybrid Synchronization**: Under sustained load, 100% of operations execute in user space with zero system calls and zero hardware memory fences on the commit/release fast path:
  - **Tier 0 (Micro-Turnaround)**: Bounded user-space micro-pause (48 `pause`/`yield` cycles ≈ 400 ns) catching in-flight commits.
  - **Tier 1 (Hardware-Assisted Wait)**: User-level monitor-wait using **Intel WAITPKG** (`UMONITOR` + `UMWAIT` in low-latency C0.1 sub-state) and **ARM64 FEAT_WFxT** (`WFET`), delivering 40–70 ns hardware wakeups with zero syscall overhead.
  - **Tier 2 (Asymmetric Dekker & Linux 5.16+ `futex2`)**: Eliminates CPU store buffer stalls (`MFENCE`) on fast paths via **Asymmetric Dekker Synchronization** (`sys_membarrier(2)` on waiter registration). Implements native **Linux 5.16+ `futex2` (`sys_futex_waitv`)** vectorized multi-wait directly on sequence words and cooperative shutdown flags.
  - **Tier 3 (Deep Passive Suspension)**: Deterministic passive `SYS_futex` sleep with dual-word atomic `FUTEX_WAKE_OP` operations, dropping idle CPU consumption to **0.3%**.
- **Configurable Multi-Tier Trust Hierarchy**:
  - **Tier 1 (Zero-Trust Default)**: Full process isolation, PID tracking, slot poisoning, and `pidfd_open(2)` crash recovery.
  - **Tier 2 (Trusted Mode, `RBIPC_FLAG_TRUSTED`)**: Bypasses PID tracking, achieving pure Vyukov turn sequence parity with DPDK `rte_ring` (**15.03 Mops/s**, +19.2% over DPDK).
  - **Tier 3 (Packed Mode, `RBIPC_FLAG_PACKED`)**: Compresses sequence turn, payload length, and slot state into a single 64-bit atomic integer for single-instruction commit and release.
  - **Combined Mode (`RBIPC_FLAG_TRUSTED | RBIPC_FLAG_PACKED`)**: Optimal small-message configuration delivering **65 ns min** and **89 ns p50** half-RTT latency.
- **B-Queue Vector Batching & SIMD Scanning**: Amortizes atomic ticket claims and memory fences across batches of contiguous slots (`rbipc_reserve_write_batch`, `rbipc_read_acquire_batch`), accelerated by 256-bit x86-64 AVX2, SSE4.1, and 128-bit ARM64 NEON sequence scanners (**54.59M msgs/s**).
- **Non-Temporal Streaming Stores**: Transfers bulk payloads ($\ge 256$ B) directly through CPU Write-Combining buffers via `_mm256_stream_si256` / `_mm_stream_si128` + `_mm_sfence()`, preventing L1/L2/L3 cache pollution and Read-For-Ownership (RFO) bus transactions.
- **Race-Free Crash Resilience & Time-Bounded Consumer Leases**: Uses Linux 5.3+ `pidfd_open(2)` to audit slot-holding peers without PID-recycling races. Automatically poisons abandoned slots (`RBIPC_SLOT_POISONED`), heals orphaned ticket holes, and enforces nanosecond consumer leases (`rbipc_set_lease_timeout`).
- **Memory Integrity & Sealing**: Seals shared memory descriptors via `fcntl(F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW)` and Linux 6.10+ `sys_mseal` to eliminate `SIGBUS` hazards.
- **Multi-Topology IPC Extensions**:
  - **Reactor Integration**: Non-blocking `eventfd(2)` readiness notifications (`rbipc_enable_eventfd`, `rbipc_eventfd_drain`) for `epoll(7)` and `io_uring` event loops.
  - **1-to-N Multicast Pub-Sub**: Wait-free topic publishing (`rbipc_publish`, `rbipc_publish_batch`) with independent subscriber cursors (`rbipc_sub_t`) and automatic lag clamping.
  - **Capability FD Passing**: Zero-namespace `memfd_create` descriptor transmission over `AF_UNIX` `SCM_RIGHTS` (`rbipc_ipc_send_fds`, `rbipc_ipc_recv_fds`).

---

## Repository Structure & Module Decomposition

```text
librbipc/
├── include/                 # Public C23/C++ ABI Interfaces
│   ├── rbipc.h              # Public ABI: core ring, zero-copy I/O, streaming, reactor, leases
│   ├── rbipc_ipc.h          # Public ABI: UNIX domain socket SCM_RIGHTS descriptor passing
│   └── rbipc_pubsub.h       # Public ABI: 1-to-N lock-free multicast publish-subscribe engine
├── src/                     # Modularized Internal Implementation Units
│   ├── audit/               # Fault tolerance, liveness auditing, and error handling
│   │   ├── rbipc_audit.c/.h # pidfd_open(2) process auditing, slot poisoning, ticket-hole healing
│   │   └── rbipc_error.c/.h # Error status formatting and system errno translation
│   ├── core/                # Core ring buffer lifecycle and slot state machines
│   │   ├── rbipc_internal.h # Process-local runtime handle representation (struct rbipc_ring)
│   │   ├── rbipc_predicate.h# Side-effect-free invariant validation predicates
│   │   ├── rbipc_ring.c/.h  # Handle allocation, layout probe, and cooperative shutdown
│   │   ├── rbipc_slot.c/.h  # Vyukov sequence state machine and consumer release
│   │   └── rbipc_waitfree.c/.h # Nikolaev wCQ bounded wait-free helping engine with O(P) step bound
│   ├── io/                  # High-throughput zero-copy I/O and vector scanning
│   │   ├── rbipc_batch.c/.h # B-Queue scatter-gather vector batch reservation/commit
│   │   ├── rbipc_dma.c/.h   # Intel DSA hardware DMA offload and vector streaming fallback
│   │   ├── rbipc_io.c/.h    # Fast-path ticket reservation, address calculation, and copy wrappers
│   │   ├── rbipc_reactor.c/.h# Linux eventfd(2) / epoll notification reactor
│   │   ├── rbipc_simd.c/.h  # Hardware-accelerated AVX2, SSE4.1, and NEON sequence scanners
│   │   └── rbipc_uring.c/.h # Linux io_uring fixed buffer zero-copy registration and SQPOLL ring
│   ├── ipc/                 # Inter-process transport integration
│   │   ├── rbipc_ipc.c      # SCM_RIGHTS socket passing for file descriptors
│   │   └── rbipc_pubsub.c   # Lock-free 1-to-N publish-subscribe multicast broker
│   ├── memory/              # Virtual memory and cache-bypassing streaming
│   │   ├── rbipc_cxl.c/.h   # Compute Express Link (CXL) DAX mapping and CLWB writebacks
│   │   ├── rbipc_mpk.c/.h   # Intel MPK/PKU user-space hardware protection key domain isolation
│   │   ├── rbipc_shm.c/.h   # POSIX shm / Linux memfd_create, layout geometry, F_ADD_SEALS
│   │   ├── rbipc_stream.c/.h# AVX-512, AVX2, and SSE2 non-temporal streaming store engines
│   │   └── rbipc_vmem.c/.h  # Dual-mapped mirroring and Linux 6.10+ mseal virtual memory sealing
│   ├── platform/            # Architecture abstractions and hardware builtins
│   │   ├── rbipc_arch.h     # vDSO monotonic clock, CPU pause/yield, hardware CRC32C
│   │   ├── rbipc_attr.h     # Compiler optimization macros (RBIPC_INLINE, PREFETCH)
│   │   └── rbipc_math.h     # Hardware-accelerated bitwise math (CTZ, popcount, power-of-two)
│   └── sync/                # Low-latency hybrid synchronization hierarchy
│       ├── rbipc_futex.c/.h # Linux sys_futex (FUTEX_WAIT/WAKE) and Linux 5.16+ sys_futex_waitv
│       ├── rbipc_sync.c/.h  # Asymmetric Dekker barriers, bounded micro-pause, and adaptive backoff
│       └── rbipc_uintr.c/.h # Intel User Interrupts (UINTR / SENDUIPI) direct core-to-core signaling
├── benchmarks/              # Comprehensive benchmark suites and datasets
│   ├── architecture/        # Architectural evaluation modules (hugepages, pubsub, latency)
│   ├── competitors/         # Cross-system benchmarks (librbipc vs DPDK, Disruptor, mqueue, AF_UNIX)
│   ├── deep_dive/           # Deep dive feature benchmarks (SIMD, leases, reactor, streaming)
│   ├── idle/                # Idle strategy benchmarks (passive futex vs busy-spin polling)
│   └── suite/               # Streaming throughput, payload scaling, and MPMC contention suites
├── tests/                   # 31 automated verification suites across 8 subsystems (100% pass)
├── tools/                   # Diagnostic and monitoring utilities (rbipc_stat, rbipc_telemetry_stress)
├── scripts/                 # Benchmarking harnesses, documentation bundlers, and symbol maps
├── ARCHITECTURE.md          # Comprehensive architectural specification and subsystem blueprints
├── BENCHMARKS.md            # Deep empirical benchmark analysis, perf profiling, and motivations
├── Doxyfile                 # Doxygen documentation configuration
├── Makefile                 # Modular build system with strict C23 flags (-std=c23 -Werror)
├── librbipc.pc.in           # pkg-config package specification template
└── LICENSE                  # Apache 2.0 open-source software license
```

---

## Public API Quick Reference

### 1. Lifecycle & Configuration (`include/rbipc.h`)

| Function | Description |
| :--- | :--- |
| `rbipc_create` / `rbipc_create_ex` | Create an anonymous (`memfd_create`) or named (`shm_open`) ring buffer with explicit role flags. |
| `rbipc_attach` / `rbipc_attach_ex` | Attach to an existing named shared-memory ring buffer with CRC32C and layout verification. |
| `rbipc_attach_fd` / `rbipc_attach_fd_ex` | Attach directly to an inherited or socket-received (`SCM_RIGHTS`) shared-memory file descriptor. |
| `rbipc_detach` / `rbipc_destroy` | Detach handle, decrement active role counters, close descriptors, or unlink shared-memory segments. |
| `rbipc_signal_shutdown` / `rbipc_shutdown`| Set atomic shutdown flag and wake all blocked producers and consumers via `sys_futex_waitv`. |
| `rbipc_set_wait_policy` / `_get_wait_policy`| Configure wait policy (`RBIPC_WAIT_ADAPTIVE`, `RBIPC_WAIT_ZERO_SPIN`, `RBIPC_WAIT_HW_MONITOR`). |

### 2. Zero-Copy Single-Slot I/O (`include/rbipc.h`)

| Function | Description |
| :--- | :--- |
| `rbipc_reserve_write` / `_nonblock` / `_timeout` | Reserve a write ticket and obtain a direct zero-copy pointer (`void **out_buf`) to the slot buffer. |
| `rbipc_commit_write` / `rbipc_abort_write` | Publish a populated slot (`sequence = ticket + 1`), or mark an aborted slot as poisoned. |
| `rbipc_read_acquire` / `_nonblock` / `_timeout` | Acquire a read ticket and obtain a direct zero-copy pointer (`const void **out_buf`), length, and ticket. |
| `rbipc_read_release` | Recycle a consumed slot (`sequence = ticket + capacity`) and awaken waiting producers if space-blocked. |
| `rbipc_write` / `rbipc_read` | Convenience wrappers: reserve $\to$ `memcpy` $\to$ commit, and acquire $\to$ `memcpy` $\to$ release. |

### 3. B-Queue Vector Batching & Streaming Stores (`include/rbipc.h`)

| Function | Description |
| :--- | :--- |
| `rbipc_reserve_write_batch` / `_nonblock` / `_timeout` | Reserve up to `count` contiguous slots in a single atomic step, populating an array of `rbipc_iovec_t`. |
| `rbipc_commit_write_batch` | Atomically commit a batch of reserved slots with a single memory fence and batched waiter notification. |
| `rbipc_read_acquire_batch` / `_nonblock` / `_timeout` | Acquire up to `count` ready slots in a single atomic step, populating an array of `rbipc_rovec_t`. |
| `rbipc_read_release_batch` | Recycle a batch of consumed read slots in a single atomic pass. |
| `rbipc_stream_copy` | Transfer data using AVX2 (`_mm256_stream_si256`) / SSE2 non-temporal stores + `_mm_sfence()`. |
| `rbipc_write_stream` / `_nonblock` / `_timeout` | Reserve slot, stream payload directly to DRAM bypassing caches, and commit. |

### 4. Reactor, Leases, Multicast Pub-Sub, & IPC Passing

| Function | Header | Description |
| :--- | :--- | :--- |
| `rbipc_enable_eventfd` / `rbipc_eventfd_drain` | `rbipc.h` | Bind `eventfd(2)` descriptor for `epoll(7)` / `io_uring` event loop integration. |
| `rbipc_set_lease_timeout` / `_revoke_expired_leases` | `rbipc.h` | Configure nanosecond consumer leases and revoke slots held by stalled consumers. |
| `rbipc_publish` / `rbipc_publish_batch` | `rbipc_pubsub.h` | Publish single or batched topic messages in 1-to-N lock-free multicast mode. |
| `rbipc_sub_create` / `rbipc_sub_poll` / `rbipc_sub_read` | `rbipc_pubsub.h` | Manage subscriber handles with independent cursors and automatic cursor lag clamping. |
| `rbipc_ipc_send_fds` / `rbipc_ipc_recv_fds` | `rbipc_ipc.h` | Transmit up to 8 file descriptors across `AF_UNIX` sockets via `SCM_RIGHTS`. |

---

## Quick Start Example

```c
#include <rbipc.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  rbipc_ring_t *ring = NULL;

  // Create an anonymous shared memory ring buffer (1024 slots, 256 bytes per slot)
  // Configured in Combined Mode for ultra-low latency small messages
  int rc = rbipc_create_ex(NULL, 1024, 256,
                           RBIPC_SHM_ANON | RBIPC_FLAG_TRUSTED | RBIPC_FLAG_PACKED,
                           0660, &ring);
  if (rc != RBIPC_OK) {
    fprintf(stderr, "Failed to create ring: %s\n", rbipc_strerror(rc));
    return 1;
  }

  // Zero-Copy Producer: Reserve, populate directly in shared memory, commit
  void *write_buf = NULL;
  uint64_t write_ticket = 0;
  const char msg[] = "Hello from librbipc!";
  
  if (rbipc_reserve_write(ring, sizeof(msg), &write_buf, &write_ticket) == RBIPC_OK) {
    memcpy(write_buf, msg, sizeof(msg));
    rbipc_commit_write(ring, write_ticket, sizeof(msg));
  }

  // Zero-Copy Consumer: Acquire, read directly from shared memory, release
  const void *read_buf = NULL;
  uint32_t read_len = 0;
  uint64_t read_ticket = 0;

  if (rbipc_read_acquire(ring, &read_buf, &read_len, &read_ticket) == RBIPC_OK) {
    printf("Received (%u bytes): %s\n", read_len, (const char *)read_buf);
    rbipc_read_release(ring, read_ticket);
  }

  rbipc_destroy_ring(ring);
  return 0;
}
```

---

## Performance Summary

Measured empirically on reference hardware (**Intel Core i5-6300U @ 2.40 GHz**, 2C/4T Skylake Mobile, Linux `7.2.9-2-cachyos` x86_64, AC Mode). See **[`BENCHMARKS.md`](BENCHMARKS.md)** for full latency distributions, flamegraphs, and `perf c2c` cache contention analyses:

| Evaluation Dimension | `librbipc` Result | DPDK `rte_ring` | LMAX Disruptor | POSIX `mqueue` | `AF_UNIX` SOCK_DGRAM |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **1P-1C 64B Streaming (Trusted)** | **15.03 Mops/s** (917.3 MB/s) | 12.61 Mops/s | 28.29 Mops/s | 0.48 Mops/s | 0.46 Mops/s |
| **1P-1C 64B Streaming (Zero-Trust)** | **11.93 Mops/s** (728.2 MB/s) | N/A (No isolation) | N/A (No isolation) | 0.48 Mops/s | 0.46 Mops/s |
| **B-Queue Vector Batch (Batch 32)** | **54.59 Mops/s** (3,331.9 MB/s) | N/A | N/A | N/A | N/A |
| **E2E Fork Multi-Process (Batch 32)**| **48.15 Mops/s** (20.77 ns/msg) | N/A | N/A | N/A | N/A |
| **Half-RTT Ping-Pong Latency ($p50$)**| **89 ns** (Combined) / **184 ns** (ZT) | 78 ns | 43 ns | 4,274 ns (48× slower) | 5,229 ns (58× slower) |
| **Half-RTT Ping-Pong Latency (Min)** | **65 ns** (Combined) | 49 ns | 35 ns | 2,898 ns | 3,233 ns |
| **Tail Latency ($p99.9$ / Max)** | **5.55 µs / 10.34 µs** (Combined) | 0.45 µs / 525 µs | 0.15 µs / 83.5 µs | 313 µs / 991 µs | 299 µs / 645 µs |
| **Contention Robustness (4P-4C)** | **1.69 Mops/s** (Sustained) | **0.07 Mops/s (Collapsed)**| **0.04 Mops/s (Collapsed)**| < 0.2 Mops/s | < 0.2 Mops/s |
| **Idle CPU Utilization** | **0.3% CPU** (Green Compute) | 96.7% (Busy-Spin) | 96.3% (Busy-Spin) | VFS Dependent | VFS Dependent |
| **Hardware perf IPC** | **1.59 IPC** (Batch 32) | ~0.70 IPC | ~1.10 IPC | < 0.30 IPC | < 0.30 IPC |
| **Branch Misprediction Rate** | **0.03%** | ~0.5% | ~0.4% | > 2.0% | > 2.5% |
| **Cache Miss Rate** | **0.62%** (Batch 32) | ~2.5% | ~2.0% | > 8.0% | > 9.0% |

---

## Build, Test, & Installation

### Prerequisites

A modern Linux distribution with an ISO C23-compliant compiler (`gcc` $\ge 13$ or `clang` $\ge 17$), `make`, and standard POSIX development headers. Public headers (rbipc.h, rbipc_ipc.h, rbipc_pubsub.h) are natively compatible with ISO C++17, C++20, and C++23 (g++ / clang++).

### Building

```bash
# Build static library, shared library, and all verification test suites
make -j$(nproc)

# Run complete test suite across C23 and C++20 (100% pass)
make test

# Build all benchmark suites
make -j$(nproc) bench

# Run comprehensive benchmark and hardware profiling suite with perf metrics
python3 scripts/run_comprehensive_suite.py
```

### Installation

```bash
sudo make install PREFIX=/usr/local
```

---

## License

Copyright © 2026 Christian Santarelli (`Seintian`). Licensed under the [Apache License, Version 2.0](https://www.apache.org/licenses/LICENSE-2.0).
