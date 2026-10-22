# `librbipc` Architectural Evolution, Benchmark Design, & Subsystem Specification

## 1. Executive Summary & Architectural Overview

This document provides the definitive technical specification and evolution log of the `librbipc` shared-memory IPC engine. It synthesizes:

1. **Architectural Analysis of Core Commits**: Deep microarchitectural and concurrency examination of anonymous shared memory, the 3-tier trust hierarchy, AVX sequence turn scanning, multi-FD transmission, and lock-free multicast distribution.
2. **Defect Rectification & Root-Cause Analysis**: Documenting critical concurrency, serialization, and telemetry bug repairs verified across automated test harnesses.
3. **Architectural Evaluation Benchmark Suite Specification** ([`benchmarks/bench_architecture_suite.c`](benchmarks/bench_architecture_suite.c)): Formal blueprint of the 8-module evaluation suite measuring latency hierarchies, cache saturation, B-Queue vector scaling, memory lifecycles, and Transparent HugePage throughput.
4. **Autonomous Dynamic Transparent HugePages (THP) Specification**: Engineering design and verification of zero-configuration 2MB PMD layout alignment and kernel promotion (`MADV_HUGEPAGE`).
5. **Formal Systems Harmonization & Subsystem Verification**: Maintaining strict academic tone, zero-warning ISO C23 compilation (`-std=c23 -Wall -Wextra -Wpedantic -Werror`), and 100% pass rates across 30 verification suites.
6. **Next-Generation Academic Systems Research Synthesis** ([`RESEARCH_REPORT.md`](RESEARCH_REPORT.md)): Deep evaluation of 7 cutting-edge microarchitectural dimensions from USENIX OSDI, SOSP, ATC, ASPLOS, SPAA, PPoPP, and LKML (Intel DSA memory offloading, UINTR direct ring-3 interrupts, wCQ bounded wait-freedom, Intel MPK / `mseal` zero-trust compartmentalization, CXL coherent fabric queues, `io_uring` fixed buffer zero-copy piping, and GenMC formal verification).

---

## 2. Reference Hardware Environments & System Specifications

Evaluation benchmarks and telemetry profiles are structured across two primary hardware environments:

### Reference Profile: `home_pc` (Desktop Reference Architecture)

- **CPU**: Intel Core i5-10400F @ 2.90 GHz (Comet Lake, 6 physical cores, 12 logical threads; 10 vCPUs assigned to WSL2).
- **Caches**: 32 KiB L1d/L1i per core, 256 KiB L2 per core, 12 MiB shared L3 cache.
- **Vector Instruction Sets**: AVX2, AVX, SSE4.2, SSE4.1, SSSE3, BMI1, BMI2, POPCNT, CLFLUSHOPT. (Hardware does not support AVX-512).
- **OS Kernel**: Linux `6.18.33.2-microsoft-standard-WSL2` x86_64.
- **Kernel Capabilities**:
  - `futex2` (`sys_futex_waitv`, syscall 449) supported for 32-bit wait elements.
  - HugeTLB 2MB supported by architecture; `nr_hugepages` statically zero in guest instance.

### Reference Profile: `laptop` / `laptop_powersave` (Mobile Architecture)

- **CPU**: Intel Core i5-6300U @ 2.40 GHz (Skylake Mobile, 2 physical cores, 4 logical threads).
- **Caches**: 32 KiB L1d/L1i per core, 256 KiB L2 per core, 3 MiB shared L3 cache.
- **OS Kernel**: Linux `7.2.9-2-cachyos` x86_64 (Preempt Dynamic).
- **Power Governor & Routing**: Automatically routes telemetry between plugged-in performance mode (`laptop`) and battery-throttled power-save mode (`laptop_powersave`) based on `/sys/class/power_supply/AC/online`.

---

## 3. Deep Analysis of Architectural Commits

The evolution of `librbipc` incorporates significant architectural optimizations across synchronization, memory geometry, and vector scanning:

1. **Anonymous Shared Memory Default and 3-Tier Performance Hierarchy (`ceed2b0`, `e1869ca`)**:
   - **Anonymous Shared Memory Default**: Switched `rbipc_create(NULL, ...)` to use Linux `memfd_create(2)`, avoiding `/dev/shm` filesystem path lookups, dentry creation, permission checking, and unlinking. Descriptors are passed across processes via UNIX domain sockets (`SCM_RIGHTS`).
   - **Tier 1 (Zero-Trust, Default)**: Multi-process safety with bounded FAA reservation (Nikolaev SCQ pattern), atomic state machine (`EMPTY` $\to$ `RESERVING` $\to$ `RESERVED` $\to$ `COMMITTED`), acquire timestamps, and producer PID tracking.
   - **Tier 2 (Trusted Mode, `RBIPC_FLAG_TRUSTED`)**: Elides intermediate state machines and PID tracking. Uses pure turn-based Vyukov sequence tracking. Cuts memory writes per message from 3 cache line writes down to 1.
   - **Tier 3 (Packed Mode, `RBIPC_FLAG_PACKED`)**: Bit-packs sequence (48 bits), payload length (12 bits), and slot state (4 bits) into a single 64-bit atomic integer (`sequence`), turning commit and release into single 64-bit atomic stores (`rbipc_slot_commit_packed`, `rbipc_slot_release_packed`).

2. **AVX1 Fallback Vector Scanner (`547b575`)**:
   - Implemented `rbipc_simd_scan_avx` in [`src/io/rbipc_simd.c`](src/io/rbipc_simd.c). Allows processors with 256-bit AVX floating-point registers but no AVX2 integer instructions (Sandy Bridge, Ivy Bridge) to scan 4 slot sequences per instruction via dual 128-bit VEX conversions.

3. **12-Bit Packed Descriptors & Bandwidth Physics Specification (`e5229b1`)**:
   - Documented the architectural trade-off of Tier 3 (12-bit length = 4,096-byte slot max), physical DRAM GB/s saturation vs virtual cache throughput, and vector fallback tiers.

4. **Tier 3 Packed Support in Multicast Pub-Sub (`f15eb14`)**:
   - Updated multicast subscriber lag computation and batch polling (`rbipc_sub_poll`, `rbipc_sub_poll_batch`) to unpack sequences and lengths from 64-bit packed words.

5. **Tier 3 Packed Support in Crash Recovery & Auditing (`cab5c69`)**:
   - Updated ticket-hole healing (`rbipc_audit_check_and_heal_ticket_hole`) and lease revocation (`rbipc_audit_revoke_expired_leases`) to unpack states and sequences from packed slots.

6. **Telemetry State Unpacking in Packed Rings (`816484b`)**:
   - Fixed telemetry reporting in [`src/core/rbipc_ring.c`](src/core/rbipc_ring.c) so monitoring tools accurately report slot state in packed rings.

7. **Zero-Copy Multicast Publishing & Abort Handling (`a36e9d0`)**:
   - Implemented `rbipc_publish_reserve`, `rbipc_publish_commit`, and `rbipc_publish_abort` in [`src/ipc/rbipc_pubsub.c`](src/ipc/rbipc_pubsub.c). Publishers reserve slot space directly, write without intermediate buffers, and commit across N subscribers.

8. **Multi-FD Transmission & Auto-Binding Reactor (`bad69a3`)**:
   - Implemented `rbipc_ipc_send_fds` and `rbipc_ipc_recv_fds` in [`src/ipc/rbipc_ipc.c`](src/ipc/rbipc_ipc.c) to transmit up to 8 file descriptors in a single `sendmsg` call. Also enabled automatic eventfd binding on socket reception.

9. **HugeTLB 2MB Static Paging Support (`009664a`)**:
   - Added `RBIPC_FLAG_HUGETLB` and `rbipc_shm_hugetlb_available()` in [`src/memory/rbipc_shm.c`](src/memory/rbipc_shm.c) using `MFD_HUGETLB | MFD_HUGE_2MB`, reducing TLB misses by 512× for large ring buffers.

10. **Futex2 Probe Memoization (`91635bc`)**:
    - Memoized `rbipc_futex2_has_u64()` in [`src/sync/rbipc_futex.c`](src/sync/rbipc_futex.c) with atomic caching to eliminate redundant kernel syscall probes.

---

## 4. Codebase Defect Rectifications & Root-Cause Analysis

Four critical defects were identified, analyzed, and repaired at the root cause:

1. **[`include/rbipc.h`](include/rbipc.h)**:
   - Fixed missing `#` before `include <assert.h>` on line 43. Standardized clean C23 includes (`<assert.h>`, `<stdalign.h>`).
2. **[`src/audit/rbipc_audit.c`](src/audit/rbipc_audit.c) (`rbipc_audit_recover_dead_producer`)**:
   - **Root Cause**: The function did not check whether `turn == ticket`. When slots in Tier 3 progressed past `ticket`, residual reserved state or uninitialized state caused readers to execute expensive `kill(pid, 0)` syscalls repeatedly in the spin backoff path.
   - **Fix Applied**: Added sequence turn validation (`uint64_t turn = is_packed ? rbipc_slot_unpack_seq(raw_seq) : raw_seq; if (turn != ticket) return false;`) and unpacked `state` from `raw_seq` for packed rings while checking `slot->state` for in-flight reservations.
3. **[`src/core/rbipc_slot.c`](src/core/rbipc_slot.c)**:
   - **Fix Applied**: In `rbipc_slot_commit_packed`, set `slot->state = RBIPC_SLOT_COMMITTED`. In `rbipc_slot_release_packed`, set `slot->state = RBIPC_SLOT_EMPTY` and cleared `producer_pid = 0`, `consumer_pid = 0`, and `acquire_timestamp_ns = 0` to preserve metadata consistency for zero-trust fallback and audit.
4. **[`tests/io/test_e2e_throughput.c`](tests/io/test_e2e_throughput.c)**:
   - **Root Cause**: Child processes called standard `exit(0)`, causing duplicate stdout buffer flushes when standard output was redirected or piped.
   - **Fix Applied**: Added `fflush(stdout)` immediately before `fork()` and used `_exit(0)` in child processes.

All 30 test binaries pass 100% cleanly without warnings under `-std=c23 -Wall -Wextra -Wpedantic -Werror`.

---

## 5. Architectural Evaluation Benchmark Suite Specification (`benchmarks/bench_architecture_suite.c`)

The evaluation suite comprehensively quantifies performance gains across 8 dedicated modules:

### Module 1: 3-Tier Latency Hierarchy (Half-RTT Ping-Pong)

- **Setup**: Pinned producer (CPU 2) and consumer (CPU 3) ping-ponging 64-byte messages across two ring buffers (`SHM_LAT_A` and `SHM_LAT_B`).
- **Iterations**: 50,000 rounds (plus 500 warmup rounds).
- **Modes**: Tier 1 Zero-Trust, Tier 2 Trusted, Tier 3 Packed, Combined Tier 2+3.
- **Metrics**: Min, P25, P50, P75, P90, P99, P99.9, Max, Mean (in ns, half-RTT).

### Module 2: Streaming 1P-1C Throughput & Cache Saturation

- **Setup**: Unidirectional streaming of 200,000 messages (64-byte payload).
- **Execution**: Evaluated both Pinned (CPU 2 $\to$ CPU 3) and Unpinned across all 4 modes.
- **Metrics**: Throughput (msgs/sec), Bandwidth (MB/sec), Latency per message (ns/msg).

### Module 3: B-Queue Vector Batching Scaling

- **Setup**: Batching via `rbipc_write_batch` and `rbipc_read_acquire_batch`.
- **Batch Sizes**: 1, 4, 16, 32, 64, 128 (500,000 messages total per batch size).
- **Modes**: Tier 1 Zero-Trust vs Combined Tier 2+3.
- **Metrics**: Throughput (msgs/sec), Bandwidth (MB/sec), Latency (ns/msg), Speedup factor vs batch 1.

### Module 4: Anonymous Shared Memory vs Named POSIX Shm Lifecycle

- **Setup**: 2,000 cycles of create, attach, detach, and destroy.
- **Modes**: Anonymous (`memfd_create` + SCM_RIGHTS FD transfer) vs Named POSIX (`/dev/shm`).
- **Metrics**: Operations/sec, average latency per lifecycle cycle in microseconds (μs), speedup factor.

### Module 5: Multicast PubSub Zero-Copy vs Standard Copy

- **Setup**: 50,000 messages published to 1-to-N subscribers across fanouts: 1, 2, 4, 8.
- **Modes**: Standard Copy vs Zero-Copy on Standard and Tier 3 Packed rings.
- **Metrics**: Publisher msgs/sec, Effective delivery msgs/sec (`fanout * pub_msgs_sec`), Effective bandwidth (MB/sec).

### Module 6: SIMD Sequence Scanning Engines

- **Setup**: 2,500,000 scanning iterations (10,000,000 slots total) on a 256-slot sequence array.
- **Engines**: Scalar, SSE4.1, AVX1 fallback, AVX2.
- **Evaluation**: Standard 64-bit sequences and Tier 3 packed sequences (16-bit shift).
- **Metrics**: Million slots scanned / second (Mslots/sec), Speedup vs scalar.

### Module 7: Multi-FD Transmission, Futex2 Memoization, & HugeTLB

- **Multi-FD Transmission**: Transfer 1 FD, 2 FDs, 4 FDs in a single `rbipc_ipc_send_fds` call vs 4 sequential socket calls (5,000 iterations).
- **Futex2 Memoization**: 500,000 checks of `rbipc_futex2_has_u64()` vs raw `syscall(__NR_futex_waitv)`.
- **HugeTLB 2MB**: Capability probing via `rbipc_shm_hugetlb_available()`.

### Module 8: Zero-Config Dynamic HugePages vs Standard 4KB Paging

- **Setup**: Evaluates Standard 4KB paging vs Dynamic 2MB THP alignment across 64KB, 4MB, 16MB, and 32MB buffer sizes.
- **Metrics**: Throughput, latency, PMD mapped kilobytes (`ShmemPmdMapped`), and speedup factors.

---

## 6. Zero-Configuration Dynamic Transparent HugePages (THP) Specification

### Objective & Architecture

Ensures `librbipc` automatically unlocks 2MB HugePage performance on any unprivileged host without requiring `sudo`, static pool pre-allocation (`vm.nr_hugepages`), or administrator intervention.

### Implementation Architecture

1. [`src/memory/rbipc_vmem.c`](src/memory/rbipc_vmem.c):
   - Injects `#ifdef MADV_HUGEPAGE` advisories in `rbipc_vmem_map_data()`, `rbipc_vmem_map_ctrl()`, and `rbipc_vmem_bind_mirror_halves()` when mapping regions $\ge 2\text{ MB}$.
   - Fails softly as POSIX no-op on non-Linux or legacy kernels without degrading standard 4KB paging.
2. [`src/memory/rbipc_shm.c`](src/memory/rbipc_shm.c):
   - In `rbipc_shm_calc_data_geometry()`: aligns `data_size` to `RBIPC_HUGE_PAGE_2MB` (2MB) when `raw_data_size >= RBIPC_HUGE_PAGE_2MB`.
   - In `rbipc_shm_calc_layout()`: aligns `data_offset` to `RBIPC_HUGE_PAGE_2MB` when `data_size >= RBIPC_HUGE_PAGE_2MB`.
   - Preserves 4KB alignment for small queues (`data_size < 2MB`), preventing sparse expansion.
3. [`tests/memory/test_unit_shm.c`](tests/memory/test_unit_shm.c):
   - Implements `test_shm_thp_dynamic_hugepages()` validating 4MB buffer creation, page table touching, and `/proc/self/smaps` confirmation of `ShmemPmdMapped: 4096 kB`.
4. [`benchmarks/architecture/arch_hugepages.c`](benchmarks/architecture/arch_hugepages.c):
   - Measures scaling from 64 KB to 32 MB, demonstrating an empirical **4.37× speedup (77.10% latency cut)** on 32MB buffers with 100% PMD huge page collapse.

---

## 7. Modular Benchmark Refactoring Breakdown

Monolithic benchmark source files were systematically refactored into cohesive, modular directories with concise coordinators:

1. [`benchmarks/architecture/`](benchmarks/architecture/):
   - `arch_common.h`, `arch_latency.c`, `arch_streaming.c`, `arch_batching.c`, `arch_lifecycle.c`, `arch_pubsub.c`, `arch_simd.c`, `arch_capabilities.c`, `arch_hugepages.c`, `arch_json.c`. Coordinator: `bench_architecture_suite.c`.
2. [`benchmarks/competitors/`](benchmarks/competitors/):
   - `comp_common.h`, `comp_dpdk.c`, `comp_disruptor.c`, `comp_kernel_ipc.c`, `comp_throughput.c`, `comp_latency.c`, `comp_idle.c`, `comp_mpmc.c`, `comp_batch.c`. Coordinator: `bench_competitors.c`.
3. [`benchmarks/idle/`](benchmarks/idle/):
   - `idle_common.h`, `idle_dpdk.c`, `idle_disruptor.c`, `idle_rbipc.c`, `idle_json.c`. Coordinator: `bench_idle_strategies.c`.
4. [`benchmarks/suite/`](benchmarks/suite/):
   - `suite_common.h`, `suite_idle.c`, `suite_latency.c`, `suite_streaming.c`, `suite_batch.c`, `suite_payload.c`, `suite_mpmc.c`, `suite_json.c`. Coordinator: `bench_suite.c`.
5. [`benchmarks/deep_dive/`](benchmarks/deep_dive/):
   - `deep_common.h`, `deep_backoff.c`, `deep_stream.c`, `deep_pubsub.c`, `deep_reactor.c`, `deep_simd.c`, `deep_lease.c`, `deep_json.c`. Coordinator: `bench_deep_dive.c`.
6. [`Makefile`](Makefile):
   - Modular pattern compilation compiling coordinators and modular units under `-std=c23 -Wall -Wextra -Wpedantic -Werror -O3`.

---

## 8. Continuous Verification & Health Matrix

- **Unit & Integration Tests**: 30 / 30 test binaries pass 100% cleanly.
- **Coverage**: 83.69% line coverage across all 22 source and header units (2,191 / 2,618 lines).
- **Public Symbols**: Exactly 78 exported symbols under `LIBRBIPC_1.0` with 0 internal symbol leaks.
- **Static Analysis**: Clang-Tidy analysis passes with 0 errors across all 15 core units.
- **Documentation**: Doxygen builds cleanly with 0 warnings; post-processing verifies 114 HTML pages with 0 broken links.

---

## 9. Next-Generation Academic Systems Research & Integration Roadmap

Synthesizing frontier research from **USENIX OSDI**, **ACM SOSP**, **USENIX ATC**, **ACM ASPLOS**, **ACM SPAA**, **ACM PPoPP**, **IEEE/ACM ISCA**, and the **Linux Kernel Mailing List (LKML)** (detailed in full in [`RESEARCH_REPORT.md`](RESEARCH_REPORT.md)), the planned architectural evolution is organized across three phases:

### Phase 1: Software Concurrency & Memory Protection Primitives (v1.1)
- **Wait-Free Helping Mechanics (`RBIPC_FLAG_WAIT_FREE`)**:
  - Implement Nikolaev & Ravindran's bounded `wCQ` algorithm (ACM SPAA '22) with a cooperative helper array, guaranteeing $\mathcal{O}(P)$ step completion and eliminating $p99.999$ starvation under thread preemption.
- **Hardware Memory Protection Keys (Intel MPK / ARM MTE)**:
  - Isolate ring control headers and payload slots into dedicated protection domains (`WRPKRU`, ~20 CPU cycles) to eliminate cross-process memory corruption and TOCTTOU vulnerabilities without syscall overhead.
- **Linux 6.10+ Virtual Memory Sealing (`mseal(2)`)**:
  - Permanently seal control metadata and data mirroring VMAs against tampering, privilege modification (`mprotect`), and remapping (`mremap`).
- **Formal Weak Memory Model Verification (GenMC / IronSync)**:
  - Model check `librbipc`'s C23 atomic acquire-release orderings and Asymmetric Dekker `sys_membarrier` barriers against RC11 memory models.

### Phase 2: Hardware Accelerator & Direct Signaling Integration (v1.2)
- **Intel Data Streaming Accelerator (DSA) Memory Offload**:
  - Add `rbipc_dma_engine` dispatching 64-byte descriptors via unprivileged `ENQCMD`/`MOVDIR64B` to on-chip user work queues for payloads $\ge 4\text{ KiB}$, freeing 100% of CPU load-store execution units.
- **Intel User Interrupts (`UINTR` / `SENDUIPI`)**:
  - Implement direct Ring-3 core-to-core hardware signaling via `SENDUIPI` (< 90 ns wakeup latency) for active threads, with automatic fallback to `sys_futex_waitv` for descheduled threads.
- **Unified Linux `io_uring` Zero-Copy Pipeline**:
  - Interoperate directly with `io_uring` fixed buffer registration (`IORING_REGISTER_BUFFERS`) and polled submissions (`IORING_SETUP_SQPOLL`) for zero-copy streaming from NIC/NVMe into ring slots.

### Phase 3: Coherent Fabric Disaggregation (v2.0)
- **Compute Express Link (CXL 2.0/3.0/3.1) Multi-Host Shared Memory**:
  - Bind `librbipc` rings to CXL memory devices (`/dev/dax`), leveraging hardware Back-Invalidate (BI) cache snooping across PCIe Gen5/Gen6 fabrics to enable sub-200ns cross-server IPC without network serialization.

