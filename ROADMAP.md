# librbipc Engineering Roadmap & Technical Modernization Plan

---

## 1. Executive Vision & Architectural Strategy

**librbipc** is a high-performance, lock-free, zero-copy inter-process communication (IPC) ring buffer designed for ISO C23 on Linux. While the library demonstrates exceptional peak throughput (up to **38.1M msgs/sec** in batch mode and **13.7M msgs/sec** in streaming mode) and maintains near-zero CPU consumption (**~0.08%**) under passive waiting, an exhaustive architectural audit has identified critical latent concurrency bugs, fundamental structural incoherences, unhandled crash states, and feature gaps.

This roadmap details every finding, defect, architectural mismatch, and enhancement opportunity identified during the system audit. It establishes a multi-phase, concrete engineering plan to transition **librbipc** from an experimental high-throughput prototype into an uncompromising, mission-critical, enterprise-grade IPC runtime suitable for aerospace, robotics, financial exchanges, and high-performance computing.

---

## Roadmap Milestones at a Glance

```txt
+----------------------------------------------------------------------------------------------------+
|                                    LIBRBIPC RELEASE MILESTONES                                     |
+------------------------------------+----------------------------------+----------------------------+
| Milestone v1.1.0                   | Milestone v1.2.0                 | Milestone v2.0.0           |
| (P0: Concurrency & Safety Fixes)   | (P1: Architectural Coherence)    | (Next-Gen High Performance)|
+------------------------------------+----------------------------------+----------------------------+
| - Fix 20ms Lost-Wakeup Race        | - Resolve Double-Mapping Paradox | - 64-Bit Monotonic Sequence|
| - Fix Batch Acquire Dead-Peer Hang | - Explicit Role Architecture     | - Two-Phase Hybrid Backoff |
| - Consumer Crash Recovery & Audit  | - Safe SHM Creation & Flags      | - SIMD Vectorized Scanning |
| - Ticket-Hole Crash Recovery       | - Batch API Completeness         | - Async eventfd & epoll    |
| - Proportional Batch Wakeups       | - Modern pidfd Process Auditing  | - Optional Pub-Sub Engine  |
| - Sanitizer Suite (ASan/TSan)      | - SCM_RIGHTS Helper Primitives   | - Non-Temporal Store Paths |
+------------------------------------+----------------------------------+----------------------------+
```

---

## Phase 1: Critical Bug Fixes & Concurrency Hardening (Priority P0)

### 1.1. Eliminate the Lost-Wakeup Race in Passive Waiter Elision

* **Severity**: Critical (Correctness & Tail-Latency Anomaly)
* **Target Files**: [`src/rbipc_io.c`](src/rbipc_io.c), [`src/rbipc_sync.c`](src/rbipc_sync.c), [`src/rbipc_sync.h`](src/rbipc_sync.h)
* **Empirical Root Cause**: In `rbipc_read_acquire_timeout` and `rbipc_reserve_write_timeout`, waiter elision checks `atomic_load(futex_waiters) > 0` before waking. If the consumer inspects `slot->sequence`, finds it not ready, and is preempted *before* incrementing `futex_waiters`, a concurrent producer can commit the slot, observe `futex_waiters == 0`, and skip the futex wakeup entirely. The consumer subsequently registers as a waiter and enters `sys_futex(FUTEX_WAIT)`, sleeping until the fallback [`RBIPC_FUTEX_PERIOD_NS`](src/rbipc_sync.h#L24) timeout (20 ms) expires. This causes p99.9 latency to spike from 700 ns to **20,158,177 ns** (~20.16 ms).

#### Concrete Remediation Plan

1. **Asymmetric Waiter Registration & Sequenced Double-Check**:
   Refactor `rbipc_sync_backoff` into an explicit two-step protocol:
   * **Step 1 (`rbipc_sync_enter_wait`)**: Atomically increment `futex_waiters` using `memory_order_seq_cst`.
   * **Step 2 (Condition Re-Evaluation)**: Re-check the slot sequence condition in `rbipc_io.c`. If the slot became ready/vacant during the registration window, immediately decrement `futex_waiters` (`memory_order_seq_cst`) and proceed without calling `sys_futex`.
   * **Step 3 (`rbipc_sync_sleep`)**: Call `sys_futex(FUTEX_WAIT)` only if the condition remains unsatisfied.
   * **Step 4 (`rbipc_sync_exit_wait`)**: Decrement `futex_waiters` (`memory_order_seq_cst`) upon wakeup.
2. **Sequential Consistency in Commits**:
   Ensure `rbipc_slot_commit` and `rbipc_slot_release` issue a `memory_order_seq_cst` fence or store before sampling `futex_waiters`, closing the Store-Load reordering window.
3. **Verification**:
   Execute `bin/bench_suite` ping-pong RTT latency test for 1,000,000 iterations under high contention; verify that max latency drops from 20.16 ms to $< 15$ µs.

---

### 1.2. Implement Dead-Peer Crash Recovery in the B-Queue Batch API

* **Severity**: Critical (Permanent Deadlock on Producer Crash)
* **Target Files**: [`src/rbipc_io.c#L634-L670`](src/rbipc_io.c#L634-L670), [`src/rbipc_slot.c`](src/rbipc_slot.c)
* **Root Cause**: While single-item `rbipc_read_acquire_timeout` calls `rbipc_io_audit_and_recover_dead_peer`, `rbipc_read_acquire_batch` lacks dead-peer auditing. If a producer crashes after reserving slots with `rbipc_reserve_write_batch`, `rbipc_read_acquire_batch` permanently blocks in `rbipc_sync_backoff` because the crashed slots are never marked `POISONED`.

#### 1.2 Concrete Remediation Plan

1. **Integrate Batch Peer Auditing**:
   Modify `rbipc_read_acquire_batch`'s retry loop: when `avail == 0` (or `avail < count`), iterate over the unready slot range $[t, t + count)$ and invoke `rbipc_io_audit_and_recover_dead_peer` on any reserved slot whose `producer_pid` is deceased.
2. **Poison Propagation in Vectors**:
   Ensure that when a batch encounter poisoned slots, `rbipc_rovec_t` marks those slots with `buf = NULL` and `len = 0` so the consumer can process or skip them without stalling the pipeline.
3. **Verification**:
   Add `tests/test_e2e_batch_crash.c` where a child process reserves a batch of 16 slots, commits 8, and is killed with `SIGKILL`. Verify that the consumer recovers the remaining 8 slots without hanging.

---

### 1.3. Implement Consumer Crash Recovery & Bidirectional Liveness Tracking

* **Severity**: Critical (Permanent Pipeline Stall on Consumer Crash)
* **Target Files**: [`include/rbipc.h`](include/rbipc.h), [`src/rbipc_slot.h`](src/rbipc_slot.h), [`src/rbipc_slot.c`](src/rbipc_slot.c), [`src/rbipc_io.c`](src/rbipc_io.c)
* **Root Cause**: `rbipc_slot_t` tracks `producer_pid` but not `consumer_pid`. If a consumer crashes after acquiring a slot but before calling `rbipc_read_release`, the slot remains locked in the `COMMITTED` sequence turn. When producers cycle through the ring buffer, they reach this slot, detect that `slot->sequence` is behind, and block forever because no consumer liveness check exists.

#### 1.3 Concrete Remediation Plan

1. **Extend Slot Descriptor**:
   Add `_Atomic uint32_t consumer_pid` and `_Atomic uint64_t acquire_timestamp_ns` to [`rbipc_slot_t`](include/rbipc.h#L186-L196) (maintaining 64-byte cacheline alignment).
2. **Track Acquiring PID**:
   In `rbipc_io_complete_read_acquisition`, record `ring->cached_pid` into `slot->consumer_pid`.
3. **Consumer Crash Audit in Producer Backoff**:
   In `rbipc_io_handle_write_backoff`, inspect the unreleased slot blocking the write turn. If its holding `consumer_pid` is dead (`kill(pid, 0) == -1 && errno == ESRCH`), execute a recovery protocol:
   * Transition the slot state to `EMPTY`.
   * Clear `consumer_pid`.
   * Advance `slot->sequence` to `ticket + capacity` (recycling the orphaned slot).
   * Wake sleeping producers.
4. **Verification**:
   Create `tests/test_e2e_consumer_crash.c`: spawn a consumer child, acquire 5 slots, kill child with `SIGKILL`, and verify that the producer detects the dead consumer, reclaims the slots, and continues streaming.

---

### 1.4. Close the Dead-Producer Ticket-Hole Allocation Race

* **Severity**: High (Unrecoverable Slot Orphan on Pre-Reservation Kill)
* **Target Files**: [`src/rbipc_io.c#L255-L259`](src/rbipc_io.c#L255-L259), [`src/rbipc_slot.c`](src/rbipc_slot.c)
* **Root Cause**: If a producer successfully increments `write_ticket` via CAS but is killed before `rbipc_io_complete_write_reservation` writes `producer_pid` and `RBIPC_SLOT_RESERVED`, the slot remains in state `EMPTY` with PID 0. The consumer's dead-peer recovery ignores it because `state != RESERVED` and `pid == 0`, causing the consumer to wait on `slot->sequence` indefinitely.

#### 1.4 Concrete Remediation Plan

1. **Pre-Reservation PID Stamping**:
   Update `rbipc_reserve_write_timeout`: store the reserving PID and an intermediate `RBIPC_SLOT_RESERVING` state into the target slot *before* or atomically with the CAS, or allow dead-peer audits to inspect tickets where `read_ticket < write_ticket` and sequence is lagging.
2. **Orphan Sequence Advance**:
   If a consumer observes `read_ticket < write_ticket` where a slot remains unreserved/uncommitted and the timestamp exceeds a timeout threshold, verify if any active producer owns the ticket. If orphaned, mark `POISONED` and advance sequence to unblock the queue.
3. **Verification**:
   Implement stress tests with thread cancellation points inserted immediately following `atomic_compare_exchange_weak` in `rbipc_io_try_claim_write_ticket`.

---

### 1.5. Prevent Destructive `shm_unlink` in `rbipc_create`

* **Severity**: High (Silent Annihilation of Running Shared Rings)
* **Target Files**: [`src/rbipc_shm.c#L157-L170`](src/rbipc_shm.c#L157-L170), [`include/rbipc.h`](include/rbipc.h)
* **Root Cause**: `rbipc_shm_create_named` unconditionally executes `shm_unlink(name)` before `shm_open(O_CREAT | O_EXCL)`. If a process calls `rbipc_create` on an existing active ring, it unlinks the live shared memory segment without warning, creating an isolated split-brain ring.

#### 1.5 Concrete Remediation Plan

1. **Respect POSIX Exclusivity**:
   Remove unconditional `shm_unlink` from `rbipc_shm_create_named`. If `shm_open(O_CREAT | O_EXCL)` fails with `EEXIST`, return `RBIPC_ERR_EXISTS` (or `RBIPC_ERR_BUSY`).
2. **Introduce Explicit Flags**:
   Add a flags bitmask parameter `uint32_t flags` to `rbipc_create_ex`:
   * `RBIPC_CREATE_EXCL`: Fail if the ring buffer already exists (safe default).
   * `RBIPC_CREATE_OVERWRITE`: Explicitly unlink and recreate if stale.
3. **Verification**:
   Update `tests/test_unit_lifecycle.c` to verify that creating an existing ring buffer without overwrite flags returns an error and preserves the original ring intact.

---

### 1.6. Proportional Wakeup Dispatch for Vector Batch Operations

* **Severity**: Medium (Starvation of Sleeping MPMC Workers)
* **Target Files**: [`src/rbipc_io.c#L569-L571`](src/rbipc_io.c#L569-L571), [`src/rbipc_io.c#L703-L705`](src/rbipc_io.c#L703-L705), [`src/rbipc_sync.c`](src/rbipc_sync.c)
* **Root Cause**: When committing or releasing a batch of $N$ items (e.g., $N = 32$ or $128$), `rbipc_commit_write_batch` and `rbipc_read_release_batch` invoke `rbipc_sync_wake_one(&hdr->futex_seq, &hdr->futex_waiters)`, passing `count = 1` to `sys_futex`. Only one sleeping thread is awakened, leaving other threads sleeping despite massive buffer capacity availability.

#### 1.6 Concrete Remediation Plan

1. **Proportional Wakeup Dispatch**:
   Add `rbipc_sync_wake_n(_Atomic uint32_t *futex_word, _Atomic uint32_t *futex_waiters, uint32_t n)` to `rbipc_sync.h`.
2. **Apply to Batch Operations**:
   In `rbipc_commit_write_batch`, call `rbipc_sync_wake_n(..., count)`. In `rbipc_read_release_batch`, call `rbipc_sync_wake_n(..., count)`.
3. **Verification**:
   Create a benchmark with 8 waiting consumers; commit a batch of 32 messages and verify all 8 consumers wake up concurrently without waiting for sequential 1-by-1 cascades.

---

## Phase 2: Architectural Coherence & Structural Cleanups (Priority P1)

### 2.1. Resolve the Double-Mapping Mirror Paradox

* **Target Files**: [`src/rbipc_vmem.c`](src/rbipc_vmem.c), [`src/rbipc_vmem.h`](src/rbipc_vmem.h), [`src/rbipc_io.h`](src/rbipc_io.h), [`src/rbipc_shm.c`](src/rbipc_shm.c)
* **Incoherence**: In `librbipc`, slots are indexed via `(ticket & mask) * slot_size`. Every slot is completely contained within the first virtual memory range $[0, \text{data\_size})$. The second contiguous `mmap` mirror half $[ \text{data\_size}, 2 \times \text{data\_size} )$ is never accessed or referenced by any code path. Furthermore, page-alignment padding separates the final slot from the boundary, meaning wrapping across the boundary would hit unused padding rather than slot 0.

#### Resolution Strategy (Selectable Architecture)

* **Option A: Pure Slot-Based Optimization (Lean Mode)**:
  Eliminate the second `mmap` call and anonymous reservation in `rbipc_vmem_map_double`. Allocate a single contiguous $1\times$ data mapping.
  * *Benefit*: Cuts page-table overhead by 50%, reduces virtual address fragmentation, and simplifies initialization.
* **Option B: True Variable-Length Byte-Stream Mode (Dual Engine)**:
  Retain the double-mapping mirror for an explicit **Byte-Stream API** (`rbipc_stream_write`, `rbipc_stream_read`) that allows arbitrary non-chunked variable-size payloads to cross the circular buffer boundary seamlessly without slot fragmentation.
* **Action**:
  Refactor `librbipc` to use Option A for `rbipc_ring` (slot mode) and implement Option B as a dedicated `rbipc_stream` abstraction.

---

### 2.2. Decouple Participant Roles from Creator Status

* **Target Files**: [`include/rbipc.h`](include/rbipc.h), [`src/rbipc_ring.c`](src/rbipc_ring.c)
* **Incoherence**: `rbipc_create` unconditionally assumes the creator is a Producer (`active_producers = 1`), and `rbipc_attach` unconditionally assumes the attachee is a Consumer (`active_consumers++`). In real-world topologies (e.g., a supervisory daemon initializing the IPC fabric for separate worker processes), this corrupts telemetry and participant accounting.

#### 2.2 Concrete Remediation Plan

1. **Explicit Role Enumeration**:

   ```c
   typedef enum {
       RBIPC_ROLE_PRODUCER = (1 << 0),
       RBIPC_ROLE_CONSUMER = (1 << 1),
       RBIPC_ROLE_MONITOR  = (1 << 2)  /* Read telemetry only; does not affect active counts */
   } rbipc_role_t;
   ```

2. **Add Extended Attachment API**:
   Implement `rbipc_create_ex` and `rbipc_attach_ex` accepting `uint32_t role_flags`.
3. **Accurate Counter Tracking**:
   Increment and decrement `active_producers` and `active_consumers` strictly based on declared roles rather than creator status.

---

### 2.3. Align Benchmark Comparisons & Formalize Delivery Semantics

* **Target Files**: [`BENCHMARK_ICEORYX2_COMPARISON.md`](BENCHMARK_ICEORYX2_COMPARISON.md), [`README.md`](README.md)
* **Incoherence**: `librbipc` is an **MPMC Competing-Consumer Work Queue** (where each message is consumed by exactly one reader), yet `BENCHMARK_ICEORYX2_COMPARISON.md` compares it against Eclipse `iceoryx2`'s **Publish-Subscribe Multicast** engine (where every message is delivered to all subscribers).

#### 2.3 Concrete Remediation Plan

1. **Document Delivery Semantics Explicitly**:
   Update `README.md` and API documentation to clarify that `librbipc` implements competing-consumer point-to-point / work-distribution queueing semantics.
2. **Update Benchmark Comparative Disclaimers**:
   Add an architectural caveat in `BENCHMARK_ICEORYX2_COMPARISON.md` highlighting the semantic differences between multicast topic distribution and competing-consumer queue dispatch.
3. **Benchmark Against Equivalent Queue Paradigms**:
   Add comparative benchmarks against DPDK `rte_ring`, Linux POSIX `mqueue`, and ZeroMQ in addition to iceoryx2.

---

### 2.4. Clarify File Sealing Limitations on POSIX Shared Memory

* **Target Files**: [`src/rbipc_shm.c#L221-L232`](src/rbipc_shm.c#L221-L232), [`README.md`](README.md), [`include/rbipc.h`](include/rbipc.h)
* **Incoherence**: Documentation claims POSIX shared memory objects are sealed with `F_ADD_SEALS`. However, the Linux kernel only supports file sealing on `memfd_create` descriptors; calling `fcntl(F_ADD_SEALS)` on `shm_open` `/dev/shm` files fails with `EINVAL` (which `rbipc_shm_seal` silently ignores).

#### 2.4 Concrete Remediation Plan

1. **Accurate Error Reporting**:
   Update `rbipc_shm_seal` to return `RBIPC_ERR_NOTSUP` if `fcntl(F_ADD_SEALS)` fails on a non-memfd descriptor.
2. **Promote `memfd` + `SCM_RIGHTS` as the Primary Secure Mode**:
   Highlight anonymous `memfd` as the recommended security default for sealed, zero-truncation environments, while documenting named `/dev/shm` as an unsealed fallback.

---

### 2.5. Eliminate Empty Header: `rbipc_error.h`

* **Target Files**: [`src/rbipc_error.h`](src/rbipc_error.h)
* **Incoherence**: `src/rbipc_error.h` defines no types, prototypes, or macros; it merely includes `rbipc.h` and `rbipc_attr.h`.

#### 2.5 Concrete Remediation Plan

1. Delete `src/rbipc_error.h` and update internal includes, or populate it with internal diagnostic error codes and formatting helpers.

---

## Phase 3: Functional Surface & API Completeness (Priority P1)

### 3.1. Complete the B-Queue Vector Batch API Matrix

* **Target Files**: [`include/rbipc.h`](include/rbipc.h), [`src/rbipc_io.c`](src/rbipc_io.c)
* **Gap**: Single-item operations support blocking, timeout, and non-blocking modes. Vector batch operations only support unbounded blocking (`UINT64_MAX`).

#### New API Surface

```c
/* Non-blocking batch reservation and acquisition */
int rbipc_reserve_write_batch_nonblock(rbipc_ring_t *ring, uint32_t count,
                                      rbipc_iovec_t *iovecs, uint32_t *out_reserved);
int rbipc_read_acquire_batch_nonblock(rbipc_ring_t *ring, uint32_t count,
                                     rbipc_rovec_t *rovecs, uint32_t *out_acquired);

/* Timeout-bounded batch reservation and acquisition */
int rbipc_reserve_write_batch_timeout(rbipc_ring_t *ring, uint32_t count, uint64_t timeout_ns,
                                     rbipc_iovec_t *iovecs, uint32_t *out_reserved);
int rbipc_read_acquire_batch_timeout(rbipc_ring_t *ring, uint32_t count, uint64_t timeout_ns,
                                    rbipc_rovec_t *rovecs, uint32_t *out_acquired);
```

---

### 3.2. Asynchronous Event Loop & Reactor Integration (`eventfd`, `epoll`, `io_uring`)

* **Target Files**: [`include/rbipc.h`](include/rbipc.h), [`src/rbipc_ring.c`](src/rbipc_ring.c), [`src/rbipc_io.c`](src/rbipc_io.c)
* **Gap**: Applications built on asynchronous event loops (libuv, Tokio, ASIO, Seastar) cannot integrate `librbipc` without dedicating an OS worker thread to blocking `sys_futex` calls.

#### 3.2 Concrete Remediation Plan

1. **Configurable Notification Signaler**:
   Add an optional `eventfd` notification channel in the ring configuration (`int rbipc_enable_eventfd(rbipc_ring_t *ring, int *out_eventfd)`).
2. **Dual-Trigger Notification**:
   When consumers or producers sleep, write `1` to the `eventfd` upon commit/release if active waiters exist.
3. **Reactor Friendliness**:
   Allows external frameworks to monitor ring readiness with standard `epoll_wait` or `io_uring` polling.

---

### 3.3. 64-Bit Monotonic Sequence Numbers

* **Target Files**: [`include/rbipc.h`](include/rbipc.h), [`src/rbipc_math.h`](src/rbipc_math.h), [`src/rbipc_io.c`](src/rbipc_io.c), [`src/rbipc_slot.h`](src/rbipc_slot.h)
* **Gap**: `write_ticket`, `read_ticket`, and `slot->sequence` are 32-bit integers. At 38.1M msgs/sec, a 32-bit counter wraps around in **112.7 seconds** ($\approx 1.88$ minutes). While RFC 1982 handles modulo arithmetic, a preempted thread stalled across wrap-around boundaries risks sequence corruption.

#### 3.3 Concrete Remediation Plan

1. **Transition to 64-Bit Tickets**:
   Change tickets and sequences to `_Atomic uint64_t`. At 40M msgs/sec, a 64-bit integer will not overflow for **14,600 years**.
2. **Decouple Futex Word**:
   Maintain `futex_seq` as an independent 32-bit atomic word strictly for `sys_futex` compatibility, while all queue logic operates on 64-bit integers.

---

### 3.4. Robust Process Auditing via Linux `pidfd_open`

* **Target Files**: [`src/rbipc_slot.c#L48-L55`](src/rbipc_slot.c#L48-L55), [`src/rbipc_slot.h`](src/rbipc_slot.h)
* **Gap**: `kill(pid, 0)` is vulnerable to PID recycling: if a process crashes and the OS reassigns the same PID to an unrelated new process, `kill(pid, 0)` returns 0, falsely reporting the dead peer as alive. It also fails across distinct Docker PID namespaces.

#### 3.4 Concrete Remediation Plan

1. **Linux 5.3+ `pidfd` Support**:
   When available, obtain a process file descriptor using `syscall(SYS_pidfd_open, pid, 0)`. Check liveness by polling the `pidfd` (a readable `pidfd` indicates process termination).
2. **Graceful Fallback**:
   Fallback to `kill(pid, 0)` on older kernels ($\le 5.2$).

---

### 3.5. Configurable Shared Memory Permissions & umask Enforcement

* **Target Files**: [`src/rbipc_shm.c#L163`](src/rbipc_shm.c#L163), [`include/rbipc.h`](include/rbipc.h)
* **Gap**: Permissions are hardcoded to `0660`. A restrictive process `umask` (e.g., `0077`) overrides this to `0600`, preventing group processes from attaching.

#### 3.5 Concrete Remediation Plan

1. **Accept Mode Parameter**:
   Add `mode_t mode` parameter to creation APIs.
2. **Enforce with `fchmod`**:
   Issue `fchmod(fd, mode)` immediately following `shm_open` to override process umask and ensure deterministic file access.

---

### 3.6. Turnkey UNIX Domain Socket `SCM_RIGHTS` Helper Utilities

* **Target Files**: `include/rbipc_ipc.h` (new), `src/rbipc_ipc.c` (new)
* **Gap**: `rbipc_attach_fd` allows anonymous `memfd` attachments, but users must manually write complex socket `sendmsg`/`recvmsg` boilerplate with `struct cmsghdr` to pass file descriptors between processes.

#### 3.6 Concrete Remediation Plan

1. Implement convenience helpers:

   ```c
   int rbipc_send_fd(int socket_fd, const rbipc_ring_t *ring);
   int rbipc_recv_fd(int socket_fd, rbipc_ring_t **out_ring);
   ```

---

### 3.7. Consumer Lease Timeouts (Head-of-Line Blocking Mitigation)

* **Target Files**: [`src/rbipc_io.c`](src/rbipc_io.c), [`include/rbipc.h`](include/rbipc.h)
* **Gap**: A slow consumer that acquires ticket $T$ but stalls processing it halts all subsequent consumers and producers due to sequential queue turns.

#### 3.7 Concrete Remediation Plan

1. **Configurable Lease Deadlines**:
   Allow configuring a maximum lease timeout per slot (`lease_timeout_ns`).
2. **Automated Slot Reclamation**:
   If a consumer exceeds its lease deadline without calling `rbipc_read_release`, allow subsequent consumers or a supervisor to revoke the lease, mark the slot skipped, and advance the sequence.

---

## Phase 4: Micro-Architectural & Hardware Performance Optimizations (Priority P2)

### 4.1. Adaptive Two-Phase Spin-Then-Wait Backoff Engine

* **Target Files**: [`src/rbipc_sync.c`](src/rbipc_sync.c), [`src/rbipc_sync.h`](src/rbipc_sync.h)
* **Opportunity**: `librbipc` currently executes either zero spinning or immediate kernel `sys_futex` sleep. Under high-frequency burst messaging, immediate futex sleep incurs two kernel context switches ($\approx 1.2 - 3.5$ µs) when the peer is ready within 50–100 ns.

#### 4.1 Implementation Plan

1. Implement a two-phase hybrid backoff:
   * **Phase 1 (Micro-Spin)**: Execute a bounded loop of 32 to 64 [`rbipc_cpu_pause()`](src/rbipc_arch.h#L36) instructions ($\approx 100 - 250$ ns).
   * **Phase 2 (Passive Futex Sleep)**: If the condition is still not satisfied, fall back to passive `sys_futex` sleep.
2. Provide runtime configuration: allow applications to select between `RBIPC_WAIT_ZERO_SPIN` (0% CPU strict) and `RBIPC_WAIT_ADAPTIVE` (lowest latency).

---

### 4.2. SIMD/Vectorized Batch Slot Vacancy & Readiness Scanning

* **Target Files**: [`src/rbipc_io.c#L425-L439`](src/rbipc_io.c#L425-L439), [`src/rbipc_io.c#L583-L597`](src/rbipc_io.c#L583-L597)
* **Opportunity**: In `rbipc_io_scan_vacant_slots`, slots are scanned iteratively. Because each `rbipc_slot_t` is 64 bytes wide, scanning 16 slots touches 16 distinct cachelines.

#### 4.2 Implementation Plan

1. **Structure-of-Arrays (SoA) Optimization**:
   Store hot sequence numbers in a contiguous sequence array `_Atomic uint32_t sequences[capacity]` adjacent to the header.
2. **AVX2 / AVX-512 / NEON Comparison**:
   Use vector instructions (`_mm256_cmpeq_epi32` / `_mm512_cmpeq_epi32_mask`) to evaluate 8 to 16 sequence numbers in a single CPU cycle, accelerating batch reservations by up to $4\times$.

---

### 4.3. Cache-Bypassing Non-Temporal Streaming Stores for Large Payloads

* **Target Files**: [`src/rbipc_io.c`](src/rbipc_io.c)
* **Opportunity**: For large payloads ($> 16$ KB up to 16 MB), standard writes pollute CPU L1/L2 caches and evict hot ring metadata.

#### 4.3 Implementation Plan

1. Provide zero-copy streaming copy helpers using non-temporal store instructions (`_mm_stream_si128` / `_mm_stream_si64` / ARM `vst1q`) that write directly to DRAM, bypassing CPU cache hierarchy.

---

## Phase 5: Quality Assurance, Tooling, & Verification (Priority P1)

### 5.1. ThreadSanitizer & AddressSanitizer Targets in Makefile

* **Target Files**: [`Makefile`](Makefile)
* **Gap**: The Makefile includes `valgrind` and `clang-tidy`, but lacks native compiler sanitizers. Data races and atomic memory ordering violations in lock-free code cannot be detected by Valgrind.

#### 5.1 Implementation Plan

1. Add build targets:

   ```makefile
   tsan: CFLAGS += -fsanitize=thread -g -O1
   tsan: clean test

   asan: CFLAGS += -fsanitize=address,undefined -g -O1
   asan: clean test
   ```

---

### 5.2. Automated Crash-Injection & Chaos Testing Suite

* **Target Files**: `tests/test_e2e_chaos.c` (new)
* **Plan**: Implement an automated chaos test where parent and child processes exchange millions of messages while a supervisor randomly injects `SIGKILL`, `SIGSTOP`, and `SIGCONT` signals at random microsecond intervals. Verify that the surviving peers always recover and the ring buffer never wedges.

---

### 5.3. Latency Distribution Regression Testing

* **Target Files**: `benchmarks/bench_suite.c`
* **Plan**: Integrate strict latency percentile assertions into automated CI: ensure p99.9 does not exceed 50 µs and max latency does not exceed 100 µs on reference hardware.

---

## Complete Implementation & Prioritization Matrix

| ID | Module / Area | Description | Priority | Target Milestone | Complexity |
| :--- | :--- | :--- | :---: | :---: | :---: |
| **1.1** | `rbipc_sync` / `io` | Fix lost-wakeup race in waiter elision (eliminate 20ms spike) | **P0** | v1.1.0 | High |
| **1.2** | `rbipc_io` (batch) | Implement dead-peer crash recovery in batch acquire | **P0** | v1.1.0 | Medium |
| **1.3** | `rbipc_slot` / `io` | Implement consumer crash recovery & liveness audit | **P0** | v1.1.0 | High |
| **1.4** | `rbipc_io` / `slot` | Resolve ticket-hole race between CAS and PID assignment | **P0** | v1.1.0 | Medium |
| **1.5** | `rbipc_shm` | Remove destructive `shm_unlink` in create; add collision flags | **P0** | v1.1.0 | Low |
| **1.6** | `rbipc_sync` | Proportional futex wakeups for batch commit/release | **P0** | v1.1.0 | Low |
| **2.1** | `rbipc_vmem` | Resolve double-mapping paradox; optimize $1\times$ slot mapping | **P1** | v1.2.0 | Medium |
| **2.2** | `rbipc_ring` | Decouple participant roles (`RBIPC_ROLE_PRODUCER/CONSUMER`) | **P1** | v1.2.0 | Low |
| **2.3** | Docs / Bench | Align delivery semantics & benchmark comparisons | **P1** | v1.2.0 | Low |
| **2.4** | `rbipc_shm` | Accurately report file sealing limitations on tmpfs | **P1** | v1.2.0 | Low |
| **2.5** | `rbipc_error` | Remove redundant empty `src/rbipc_error.h` header | **P1** | v1.2.0 | Trivial |
| **3.1** | `rbipc_io` | Add batch timeouts & non-blocking functions | **P1** | v1.2.0 | Medium |
| **3.2** | `rbipc_ring` / `io` | Asynchronous `eventfd` notification & reactor support | **P1** | v1.2.0 | Medium |
| **3.3** | `rbipc_pubsub` | Optional Publish-Subscribe 1-to-N Multicast Engine | **P2** | v2.0.0 | High |
| **3.4** | `rbipc_math` / `io` | 64-bit sequence counters (eliminate 112s wrap-around) | **P1** | v2.0.0 | High |
| **3.5** | `rbipc_slot` | Modern Linux `pidfd_open` process auditing | **P1** | v1.2.0 | Medium |
| **3.6** | `rbipc_shm` | Configurable `mode_t` permissions & umask enforcement | **P1** | v1.2.0 | Low |
| **3.7** | `rbipc_ipc` | Turnkey socket `SCM_RIGHTS` descriptor passing helpers | **P1** | v1.2.0 | Medium |
| **3.8** | `rbipc_io` | Consumer lease timeouts & head-of-line blocking defense | **P2** | v2.0.0 | High |
| **4.1** | `rbipc_sync` | Adaptive two-phase spin-then-wait backoff engine | **P2** | v2.0.0 | Medium |
| **4.2** | `rbipc_io` (SIMD) | AVX2 / AVX-512 / NEON vectorized sequence scanning | **P2** | v2.0.0 | High |
| **4.3** | `rbipc_io` | Non-temporal streaming stores for large payloads | **P2** | v2.0.0 | Medium |
| **5.1** | `Makefile` | Add ThreadSanitizer (`tsan`) and ASan build targets | **P0** | v1.1.0 | Low |
| **5.2** | `tests` | Automated crash-injection & chaos stress test suite | **P1** | v1.1.0 | Medium |
| **5.3** | `benchmarks` | Automated latency percentile regression testing in CI | **P1** | v1.2.0 | Low |
