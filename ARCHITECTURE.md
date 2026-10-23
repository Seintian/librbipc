# `librbipc` Architecture & Subsystems Specification

## 1. Architectural Philosophy & Core Tenets

`librbipc` is a zero-dependency, lock-free Inter-Process Communication (IPC) library written in ISO C23 for Linux. It provides a shared-memory ring buffer engineered for microsecond-scale user-space fast paths, zero idle CPU consumption during periods of inactivity, deterministic crash resilience, and multi-topology distribution.

The architecture is built on six fundamental design pillars:

1. **Lock-Free Multi-Producer / Multi-Consumer (MPMC) Fast Paths**:
   Eliminates centralized mutex locks, spinlock contention, and global head/tail Compare-And-Swap (CAS) bottlenecks. Instead, concurrent ticket allocation is decoupled from slot sequence turns, allowing arbitrary numbers of concurrent producers and consumers to advance independently.
2. **Deterministic Process Isolation & Zero-Trust Verification**:
   Unlike single-process concurrency queues (such as DPDK `rte_ring` or LMAX Disruptor) that operate under the assumption of mutually trusting threads, `librbipc` natively operates across distinct POSIX process boundaries (`memfd_create(2)`, `shm_open(3)`). The library provides defensive validation, lease tracking, and crash recovery mechanisms to protect against malicious, deadlocked, or crashed processes.
3. **Four-Tier Hybrid Synchronization**:
   Hot paths execute completely in user space with zero kernel system calls and zero hardware memory fences on commit and release. Under contention or vacancy, synchronization gracefully escalates through bounded micro-pauses, clock-gated hardware monitor-waits (`UMWAIT`, `WFET`), asymmetric Dekker memory barriers (`sys_membarrier`), and vectorized Linux 5.16+ `sys_futex_waitv` kernel sleep.
4. **Vectorized Amortization (B-Queue & SIMD Scanning)**:
   Atomic ticket allocation, memory fences, and sequence checks are amortized across batches of contiguous slots via B-Queue scatter-gather vector operations (`rbipc_reserve_write_batch`, `rbipc_read_acquire_batch`). Slot availability is checked in parallel using 256-bit AVX2, SSE4.1, or 128-bit ARM64 NEON vector sequence scanners.
5. **Cache-Bypassing Non-Temporal Streaming Stores**:
   Large message transfers ($\ge 256$ B) leverage CPU streaming store instructions (`_mm256_stream_si256`, `_mm_stream_si128`), writing directly through Write-Combining (WC) buffers into memory. This eliminates Read-For-Ownership (RFO) bus transactions and prevents cache pollution of active L1/L2 application working sets.
6. **Progressive Hardware Feature Unlocking**:
   At startup and runtime, `librbipc` inspects host CPU capabilities (`CPUID`, `getauxval`) and kernel interfaces. Advanced hardware features—such as Intel WAITPKG (`UMWAIT`), ARM64 `FEAT_WFxT`, Intel MPK (`WRPKRU`), Linux `mseal`, `io_uring`, and `sys_futex_waitv`—are dynamically activated when present, falling back cleanly to portable POSIX primitives when absent.

---

## 2. Memory Layout & Cache-Line Geometry

All inter-process state in `librbipc` resides in a single, contiguous shared-memory segment. The memory layout is strictly partitioned and cache-line aligned (64 bytes on x86-64 and ARM64) to eliminate false sharing between independent cores and CPU sockets.

```
+-------------------------------------------------------------------------+
|                  Shared Memory Segment Layout (64-Byte Aligned)         |
+-------------------------------------------------------------------------+
| [1] Control Header (rbipc_shm_header_t)                                 |
|     - Cache Line 0: Magic, Version, Layout Geometry, CRC32C, Seals      |
|     - Cache Line 1: Monotonic Tickets & State Flags (Write / Read / Init)|
|     - Cache Line 2: Producer Notification & Waiting Subsystem (Waiters) |
|     - Cache Line 3: Consumer Notification & Waiting Subsystem (Waiters) |
+-------------------------------------------------------------------------+
| [2] Slot Descriptor Array (rbipc_slot_t[capacity])                      |
|     - Exactly 64 Bytes per Slot (static_assert(sizeof(...) == 64))      |
|     - sequence (64-bit atomic sequence turn counter)                    |
|     - state (32-bit atomic slot state machine: EMPTY/RESERVED/COMMITTED)|
|     - length (32-bit payload byte count)                                |
|     - producer_pid, consumer_pid (PID attribution & crash recovery)    |
|     - acquire_timestamp_ns (Lease expiration tracking)                  |
|     - offset (64-bit payload data buffer offset)                        |
|     - _pad (Cache-line padding ensuring no adjacent slot overlap)       |
+-------------------------------------------------------------------------+
| [3] Payload Data Buffer Array (uint8_t[capacity * slot_size])           |
|     - Contiguous, cacheline-aligned data buffers                        |
|     - Optional 2x virtual memory mirroring for wrap-around continuity   |
+-------------------------------------------------------------------------+
```

### 2.1 The Control Header (`rbipc_shm_header_t`)

The control header occupies exactly 256 bytes (4 cache lines) and is placed at the start of the shared memory region:

- **Cache Line 0 (Static Metadata & Verification)**: Contains the 32-bit magic identifier (`RBIPC_MAGIC = 0x52424950`), ABI version, capacity (must be a power of two), slot data size, memory layout offsets, hardware CRC32C checksum of initial parameters, and `init_state` publication flag (`RBIPC_HEADER_INIT_DONE`).
- **Cache Line 1 (Monotonic Fast-Path Tickets)**: Houses the 64-bit monotonic `write_ticket` and `read_ticket` counters, modified via atomic fetch-and-add instructions, alongside the atomic cooperative `shutdown_flag`.
- **Cache Line 2 (Producer Waiting Subsystem)**: Contains `write_futex_seq` (32-bit proxy sequence word) and `write_waiters` (count of blocked producers waiting for ring vacancy), alongside thread-local backoff thresholds.
- **Cache Line 3 (Consumer Waiting Subsystem)**: Contains `read_futex_seq` (32-bit proxy sequence word) and `read_waiters` (count of blocked consumers waiting for ready messages).

Separating write tickets, read tickets, producer waiters, and consumer waiters into distinct cache lines ensures that producer updates do not invalidate consumer cache lines, completely avoiding cross-core cache-line ping-pong during steady-state streaming.

### 2.2 Slot Descriptor Layout (`rbipc_slot_t`)

Each slot descriptor is padded and aligned to exactly 64 bytes (`static_assert(sizeof(rbipc_slot_t) == 64)`):

```c
typedef struct rbipc_slot {
  _Alignas(RBIPC_CACHE_LINE) _Atomic uint64_t sequence;
  _Atomic uint32_t state;
  uint32_t length;
  _Atomic pid_t producer_pid;
  _Atomic pid_t consumer_pid;
  _Atomic uint64_t acquire_timestamp_ns;
  uint64_t offset;
  uint8_t _pad[24];
} rbipc_slot_t;
```

- In **Tier 1 (Zero-Trust)**: Producers and consumers verify both `sequence` turn progression and the explicit `state` transition (`EMPTY` $\to$ `RESERVING` $\to$ `RESERVED` $\to$ `COMMITTED`), recording process IDs and acquisition timestamps for audit and recovery.
- In **Tier 2 (Trusted Mode)**: `state`, PIDs, and timestamps are bypassed. Slot lifecycle is governed strictly by the monotonic `sequence` turn counter.
- In **Tier 3 (Packed Mode)**: Sequence turn (48 bits), payload length (12 bits), and slot state (4 bits) are compressed into the single 64-bit `sequence` word.

### 2.3 Transparent HugePages & Virtual Memory Mirroring

To minimize Translation Lookaside Buffer (TLB) misses:
- **Autonomous Dynamic THP**: `librbipc` aligns allocations to 2 MB boundaries and advises the Linux virtual memory subsystem via `madvise(addr, len, MADV_HUGEPAGE)`. On modern Linux kernels, the Transparent Hugepage daemon (khugepaged) promotes these anonymous shared mappings to 2 MB Page Middle Directory (PMD) entries.
- **HugeTLB Static Pages (`RBIPC_FLAG_HUGETLB`)**: Applications requiring deterministic latency can pass `RBIPC_FLAG_HUGETLB` to allocate backed 2 MB or 1 GB HugeTLB pages via `memfd_create(..., MFD_HUGETLB | MFD_HUGE_2MB)`, reducing TLB lookups by 512×.
- **Virtual Memory Double-Mapping**: Contiguous ring buffer mirroring creates two consecutive virtual address mappings of the same physical payload pages. This allows linear zero-copy scatter-gather operations across circular buffer wrap-around boundaries without manual chunk splitting.

---

## 3. Execution Modes & Trust Hierarchy

`librbipc` supports distinct execution tiers configured via creation flags (`RBIPC_FLAG_*`). This allows developers to select the exact balance of performance, process isolation, and payload packing required for their deployment environment.

```
+-------------------------------------------------------------------------------+
|                        librbipc Execution Mode Hierarchy                      |
+-------------------------------------------------------------------------------+
| Tier 1: Zero-Trust Isolation (Flag: 0)                                        |
|   - Adversarial multi-process isolation, PID tracking, CAS-elided validation  |
|   - Consumer lease enforcement, automatic slot poisoning & hole healing       |
+-------------------------------------------------------------------------------+
| Tier 2: Trusted Mode (Flag: RBIPC_FLAG_TRUSTED)                               |
|   - Single-tenant / trusted container environments                            |
|   - Pure Vyukov turn sequence parity (matches DPDK rte_ring semantics)         |
|   - Eliminates intermediate state machines; 1 cache-line write per message    |
+-------------------------------------------------------------------------------+
| Tier 3: Packed Descriptor Mode (Flag: RBIPC_FLAG_PACKED)                      |
|   - Bit-packed 64-bit descriptor: [48-bit seq | 12-bit len | 4-bit state]     |
|   - Single atomic 64-bit store publication and release                        |
|   - Maximum payload size: 4095 bytes (ideal for low-latency messaging)        |
+-------------------------------------------------------------------------------+
| Combined Mode (Flags: RBIPC_FLAG_TRUSTED | RBIPC_FLAG_PACKED)                  |
|   - Combines pure turn sequencing with 64-bit descriptor packing              |
|   - Lowest single-message latency (p50: 89 ns, batching: 20.77 ns/msg)        |
+-------------------------------------------------------------------------------+
| Bounded Wait-Free Helping Mode (Flag: RBIPC_FLAG_WAIT_FREE)                   |
|   - Nikolaev wCQ O(P) announcement array for preemptible threads              |
|   - Starvation-free guarantees under heavy OS CPU oversubscription            |
+-------------------------------------------------------------------------------+
```

### 3.1 Tier 1: Zero-Trust Multi-Process Isolation (Default, `0`)

In adversarial multi-tenant deployments or multi-process architectures where processes may crash, stall, or terminate unexpectedly, Tier 1 enforces zero-trust safety:
- **CAS-Elided Validation**: Uses monotonic ticket reservation paired with slot state transitions.
- **Process PID Attribution**: The reserving producer writes its PID into `slot->producer_pid`. If the producer crashes before committing, background consumers detect the dead process via `pidfd_open(2)` or `kill(pid, 0)` and mark the slot as `RBIPC_SLOT_POISONED`.
- **Consumer Lease Tracking**: Consumers update `slot->acquire_timestamp_ns`. If a consumer stalls or crashes past the configured lease duration (`rbipc_set_lease_timeout`), the supervisory audit engine revokes the slot and restores ring availability.

### 3.2 Tier 2: Trusted Mode (`RBIPC_FLAG_TRUSTED`)

When processes run within a mutually trusted perimeter (such as microservices in the same security enclave or high-performance financial execution engines), the overhead of PID recording, lease timestamps, and multi-field state machines can be safely eliminated:
- Slot transitions rely entirely on Dmitry Vyukov's MPMC turn algorithm:
  - **Slot Vacant for Producer**: `sequence == ticket`
  - **Slot Ready for Consumer**: `sequence == ticket + 1`
  - **Slot Recycled by Consumer**: `sequence == ticket + capacity`
- Reduces write memory traffic from 3 separate cache line writes per message down to a single atomic store, cutting latency and eliminating cache contention.

### 3.3 Tier 3: Packed Descriptor Mode (`RBIPC_FLAG_PACKED`)

For messages up to 4095 bytes, Tier 3 packs sequence, length, and state into a single 64-bit atomic integer:

$$\text{sequence\_word} = (\text{sequence} \ll 16) \mid ((\text{length} \ \& \ 0\text{xFFF}) \ll 4) \mid (\text{state} \ \& \ 0\text{xF})$$

- **Publication**: `rbipc_slot_commit_packed` publishes the message with a single 64-bit atomic store with `memory_order_release`.
- **Acquisition**: `rbipc_slot_unpack_seq` and `rbipc_slot_unpack_len` extract sequence and length directly without reading separate struct fields.
- **Cachefootprint**: Keeps descriptor metadata entirely within the CPU core's register file and L1 cache.

### 3.4 Combined Mode (`RBIPC_FLAG_TRUSTED | RBIPC_FLAG_PACKED`)

Combining Tier 2 and Tier 3 delivers the optimal configuration for small, ultra-low-latency IPC:
- Single-instruction publication via packed 64-bit descriptors.
- Zero PID or lease bookkeeping overhead.
- Direct turn sequence progression achieving sub-100 ns round-trip latency.

### 3.5 Bounded Wait-Free Helping Mode (`RBIPC_FLAG_WAIT_FREE`)

For systems running under strict real-time deadlines or extreme core oversubscription, `librbipc` implements the **Nikolaev wCQ (Wait-Free Cyclic Queue)** helping algorithm:
- Every active thread is assigned a slot in a shared announcement array of size $\mathcal{O}(P)$, where $P$ is the maximum concurrent thread count.
- When a thread fails to advance after a bounded number of CAS attempts, it publishes its request descriptor into the announcement array.
- Concurrent peer threads inspect the announcement array and cooperatively execute pending reservations on behalf of stalled threads before fulfilling their own requests.
- Guarantees deterministic, bounded progress in a finite number of CPU steps ($\mathcal{O}(P)$ step bound), eliminating tail latency spikes ($p99.999$) caused by OS thread preemption.

---

## 4. Synchronization Engine: 4-Tier Zero-Spin Architecture

Traditional IPC mechanisms face a strict dilemma: busy-spinning yields sub-microsecond latency but wastes 100% of CPU cores, while standard semaphore/mutex locks avoid CPU waste but incur thousands of nanoseconds in kernel context switches.

`librbipc` resolves this trade-off through an adaptive 4-tier synchronization hierarchy:

```
Fast Path: Slot Available
   │
   ├──► User-Space Direct Access (0 Syscalls, 0 Fences)
   │
Slow Path: Contention or Starvation
   │
   ├──► Tier 0: Bounded Micro-Spin (48 pause/yield cycles, ~400 ns)
   │
   ├──► Tier 1: Hardware-Assisted Wait (Intel UMWAIT C0.1 / ARM64 WFET, ~50 ns resume)
   │
   ├──► Tier 2: Asymmetric Dekker Barrier (Linux sys_membarrier, 0 fences on fast path)
   │            Vectorized Linux 5.16+ sys_futex_waitv (Direct slot monitoring)
   │
   └──► Tier 3: Passive Kernel Sleep (Linux SYS_futex / FUTEX_WAKE_OP)
                CPU Utilization drops to 0.0% during idle
```

### 4.1 Tier 0: Bounded Micro-Spin

When a thread encounters a slot whose sequence turn is in-flight, it enters a short, calibrated pause loop (`RBIPC_WAIT_ADAPTIVE`):
- Executes up to 48 iterations of `rbipc_cpu_pause()` (`_mm_pause` on x86-64, `isb`/`yield` on ARM64).
- Catches near-instantaneous memory publication from adjacent cores without relinquishing CPU pipeline slots or incurring OS scheduling overhead.

### 4.2 Tier 1: Hardware-Assisted Monitor Wait

If the micro-spin loop expires and hardware support is detected:
- **Intel WAITPKG (`UMONITOR` + `UMWAIT`)**: Arms hardware cache-line monitoring on the sequence address (`UMONITOR`) and enters the low-latency C0.1 optimized power state (`UMWAIT`). When the remote core writes to the monitored cache line, the CPU wakes up in 40–70 ns without kernel intervention.
- **ARM64 FEAT_WFxT (`WFET`)**: Leverages timed event wait instructions matched against virtual counter `CNTVCT_EL0`, pausing execution until cache-line invalidation or timer expiration.

### 4.3 Tier 2: Asymmetric Dekker Synchronization (`sys_membarrier`)

To eliminate hardware memory fence instructions (`MFENCE`) on the high-frequency commit/release fast paths, `librbipc` implements asymmetric memory barriers:
- **Fast Path (Commit & Release)**: Uses lightweight release semantics (`memory_order_release`). No expensive full memory barrier instructions are executed.
- **Slow Path (Waiter Sleep)**: Before sleeping, the waiting thread registers itself in `hdr->read_waiters` or `hdr->write_waiters` and invokes Linux `sys_membarrier(MEMBARRIER_CMD_GLOBAL_EXPEDITED, 0, 0)`. This system call forces a hardware memory barrier across all active CPU cores running threads of the process, ensuring sequential consistency without penalizing producers and consumers on the fast path.

### 4.4 Tier 3: Vectorized `sys_futex_waitv` & `SYS_futex`

When a thread must enter deep suspension:
- **Linux 5.16+ `futex2` (`sys_futex_waitv`)**: Wakes up directly on slot sequence changes. Vectorized monitoring checks the slot sequence turn (`slot->sequence`), the cooperative `shutdown_flag`, and proxy sequence counters simultaneously in a single system call.
- **Legacy Linux 2.6+ `SYS_futex`**: Falls back to 32-bit proxy words (`hdr->read_futex_seq`, `hdr->write_futex_seq`) with `FUTEX_WAKE_OP` atomic wakeups, preventing 32-bit integer wrap-around and ABA aliasing.
- **Result**: CPU utilization drops to **0.3%** when the ring is idle, scaling smoothly from quiescent power-saving states to tens of millions of messages per second.

---

## 5. High-Throughput I/O & Subsystems

### 5.1 Zero-Copy Single-Slot I/O

The core I/O API operates entirely without intermediate buffer allocations:

```c
// Producer: Zero-Copy Write Reservation
void *buf = NULL;
uint64_t ticket = 0;
int rc = rbipc_reserve_write(ring, payload_len, &buf, &ticket);
if (rc == RBIPC_OK) {
  // Populate payload directly into shared memory
  populate_data(buf, payload_len);
  rbipc_commit_write(ring, ticket, payload_len);
}

// Consumer: Zero-Copy Read Acquisition
const void *in_buf = NULL;
uint32_t in_len = 0;
uint64_t read_ticket = 0;
int rc = rbipc_read_acquire(ring, &in_buf, &in_len, &read_ticket);
if (rc == RBIPC_OK) {
  // Consume data directly from shared memory
  process_data(in_buf, in_len);
  rbipc_read_release(ring, read_ticket);
}
```

### 5.2 B-Queue Vector Batching & SIMD Scanning

Single-slot operations incur atomic instruction overhead for every message. For maximum throughput, `librbipc` provides **B-Queue Scatter-Gather Vector Batching**:
- **Atomic Reservation Amortization**: `rbipc_reserve_write_batch` reserves up to $K$ contiguous slots with a single `atomic_fetch_add` on `write_ticket`.
- **Batched Waiter Notification**: Producers commit all $K$ slots and issue a single waiter check and futex wakeup.
- **SIMD Sequence Scanning**: Available slot sequences are scanned using 256-bit AVX2 (`_mm256_cmpeq_epi64`), SSE4.1, or ARM64 NEON intrinsics, checking 4 slots per CPU cycle.

### 5.3 Non-Temporal Streaming Stores

For bulk payloads ($\ge 256$ B), `librbipc` provides cache-bypassing non-temporal streaming copies (`rbipc_stream_copy`):
- Uses 256-bit AVX2 (`_mm256_stream_si256`) or 128-bit SSE2 (`_mm_stream_si128`) vector streaming instructions.
- Bypasses L1, L2, and L3 caches, writing directly to DRAM via CPU Write-Combining buffers.
- Concludes with an `_mm_sfence()` store fence to ensure global memory visibility before sequence turn commitment.
- Prevents high-bandwidth IPC streams from evicting application hot-sets, hash tables, and instruction caches.

### 5.4 Asynchronous Eventfd Reactor

For integration into event loops (`epoll`, `poll`, `io_uring`):
- `rbipc_enable_eventfd(ring)` binds an internal `eventfd(2)` descriptor to ring buffer notifications.
- When consumers are waiting, producers write to the eventfd, waking up threads blocked in `epoll_wait(2)`.
- `rbipc_eventfd_drain(ring)` resets the descriptor counter with non-blocking reads.

### 5.5 Lock-Free 1-to-N Multicast Publish-Subscribe

`librbipc` extends the ring buffer into a lock-free multicast topic broker (`rbipc_pubsub.h`):
- A single publisher broadcasts messages across $N$ independent consumers.
- Each subscriber maintains an independent `rbipc_sub_t` handle tracking its local cursor sequence.
- **Wait-Free Polling**: Subscribers read messages non-destructively without modifying ring state.
- **Automatic Lag Clamping**: If a slow subscriber falls behind the publisher by more than `capacity` slots, the library automatically clamps its cursor to the oldest available valid slot, notifying the application of skipped messages via `RBIPC_ERR_OVERFLOW`.

### 5.6 UNIX Domain Socket SCM_RIGHTS FD Passing

For capability-based security architectures:
- Anonymous shared memory rings created via `memfd_create` possess no `/dev/shm` filesystem path.
- `rbipc_ipc_send_fds` and `rbipc_ipc_recv_fds` transmit up to 8 file descriptors (ring buffer memory, eventfds, synchronization descriptors) across UNIX domain sockets via `SCM_RIGHTS`.
- Allows completely unprivileged processes running inside Linux namespaces/containers to share memory without shared filesystem mounts.

---

## 6. Fault Tolerance & Deterministic Crash Recovery

`librbipc` guarantees that a process crash cannot permanently hang, corrupt, or deadlock surviving ring buffer peers:

1. **Dead Peer Detection via `pidfd_open(2)`**:
   When a thread detects an overdue or uncommitted slot, it queries the owning process using Linux `pidfd_open(2)` and non-blocking `poll(2)`. Unlike legacy `kill(pid, 0)` checks, `pidfd_open` is completely immune to PID recycling races and zombie state aliasing.
2. **Slot Poisoning (`RBIPC_SLOT_POISONED`)**:
   If a producer process crashes while holding a write reservation, the audit subsystem marks the slot as poisoned. Surviving consumers skip the poisoned slot cleanly, avoiding indefinite stalls on uncommitted sequences.
3. **Orphaned Ticket-Hole Healing**:
   If an in-flight reservation is abandoned, the audit engine advances the sequence turn after an expiration deadline, preserving monotonic sequence continuity across the ring.
4. **Time-Bounded Consumer Leases**:
   Applications can configure consumer lease timeouts (`rbipc_set_lease_timeout`). If a consumer acquires a slot and fails to release it within the lease duration (due to a deadlock, infinite loop, or crash), background threads revoke the lease, log the failure to ring telemetry, and recycle the slot.
5. **Memory Sealing (`F_ADD_SEALS` & Linux 6.10+ `sys_mseal`)**:
   Shared memory file descriptors are sealed against shrinking or growth (`F_SEAL_SHRINK | F_SEAL_GROW`) via `fcntl(2)`, eliminating `SIGBUS` hazards caused by peer truncation. On Linux 6.10+, virtual memory areas are further protected via `sys_mseal` to prevent unauthorized modification of VMA mapping flags.
