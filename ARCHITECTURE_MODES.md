# `librbipc` Architecture & Execution Modes Guide

This document provides the definitive architectural specification of execution modes, performance tiers, memory backings, and wait policies in `librbipc`. It details the microarchitectural invariants, target operational contexts, companion function pairings, and anti-patterns for each configuration.

---

## 1. Executive Summary & Configuration Dimensions

`librbipc` implements an orthogonal, multi-dimensional configuration model allowing systems engineers to fine-tune Inter-Process Communication (IPC) across three primary axes:

```txt
                          librbipc Configuration Model
                                       |
        +------------------------------+------------------------------+
        |                              |                              |
1. Trust & Performance         2. Memory Backing              3. Wait Policy
   - Tier 1: Zero-Trust           - Anonymous memfd              - Adaptive Hybrid
   - Tier 2: Trusted              - Named POSIX (/dev/shm)       - Zero Spin (Futex)
   - Tier 3: Packed               - 2MB HugeTLB / Dynamic THP    - HW Monitor (UMWAIT/WFET)
   - Combined Tier 2+3
```

---

## 2. In-Depth Operational Taxonomy of Modes

### 2.1 Tier 1: Zero-Trust Process Isolation Mode (`flags = 0`, Default)

#### Core Microarchitectural Mechanism

Tier 1 operates under an adversarial assumption where peer processes may crash, stall indefinitely, or fail to honor protocol invariants. Every slot lifecycle transition is audited through explicit operating system process identifiers (`pid_t`), atomic state transitions (`EMPTY` $\to$ `RESERVING` $\to$ `RESERVED` $\to$ `COMMITTED` $\to$ `ACQUIRED` $\to$ `POISONED`), and monotonic lease deadlines.

```txt
       Tier 1 Slot Layout (64-byte Cache-Line Aligned)
+-----------------------+------------+------------+---------------+---------------+
| Sequence Word (64b)   | State (4b) | Len (32b)  | Prod PID (32b)| Cons PID (32b)|
+-----------------------+------------+------------+---------------+---------------+
| Acquire Timestamp (64b)            | Reserved Padded Alignment (24 bytes)       |
+------------------------------------+--------------------------------------------+
```

1. **Write Reservation (`rbipc_reserve_write`)**: Stamped with producer `pid_t` and `RESERVED` state.
2. **Write Commit (`rbipc_commit_write`)**: Writes `slot->len`, updates state to `COMMITTED`, and publishes `sequence = ticket + 1` with release semantics.
3. **Read Acquisition (`rbipc_read_acquire`)**: Stamps consumer `pid_t`, records monotonic lease timestamp, and transitions state to `ACQUIRED`.
4. **Read Release (`rbipc_read_release`)**: Clears consumer/producer ownership, resets state to `EMPTY`, and advances `sequence = ticket + capacity`.
5. **Dead-Peer Detection**: Audits peer liveness via Linux 5.3+ `pidfd_open(2)` + `poll(2)` (immune to PID recycling) or POSIX `kill(pid, 0)`. Orphaned slots from crashed producers are poisoned (`RBIPC_SLOT_POISONED`), while dead consumer slots are recovered.

#### Operational Contexts

- Multi-tenant architectures and untrusted client-server processes.
- Long-running system daemons hosting third-party plugins or microservices across privilege boundaries.
- Resilient message queues where consumer deadlocks must be automatically reclaimed via leases.

#### Paired API Functions & Features

- **Bounded Leases**: `rbipc_set_lease_timeout()`, `rbipc_revoke_expired_leases()`.
- **Reservation Rollback**: `rbipc_abort_write()` (safely poisons slot if serialization fails).
- **Crash Recovery & Forensics**: `rbipc_get_slot_stats()`, `rbipc_audit_check_and_heal_ticket_hole()`, CLI tool `bin/rbipc_stat --audit`.

#### Anti-Patterns & Caveats

- **Sub-100ns Latency Budgets**: The 13 memory stores per round-trip and lease timestamp loads impose store-buffer drain latencies incompatible with ultra-low-latency tick matching.
- **Tightly-Coupled Threads**: Running Tier 1 inside a single process adds unnecessary PID tracking overhead.

---

### 2.2 Tier 2: Trusted Mode (Pure Vyukov Sequence Synchronization, `RBIPC_FLAG_TRUSTED`)

#### 2.2 Core Microarchitectural Mechanism

Tier 2 implements Dmitriy Vyukov's turn-based MPMC sequence algorithm. It assumes cooperating processes within the same trust boundary. Process registration (`producer_pid`, `consumer_pid`) and intermediate state validation (`RESERVING`, `ACQUIRED`) are completely bypassed.

1. **Write Reservation (`rbipc_reserve_write`)**: Zero descriptor stores. Ticket claimed via wait-free `atomic_fetch_add` or bounded CAS.
2. **Write Commit (`rbipc_commit_write`)**: Relaxed store to `slot->len`, followed by atomic release store to `slot->sequence = ticket + 1` (**2 stores total**).
3. **Read Acquisition (`rbipc_read_acquire`)**: Direct integer comparison (`seq == ticket + 1`). Reads `slot->len`. Zero descriptor stores.
4. **Read Release (`rbipc_read_release`)**: Atomic release store to `slot->sequence = ticket + capacity` (**1 store total**).

```txt
Round-Trip Descriptor Stores: Tier 1 (13 stores) vs. Tier 2 (3 stores) [-77% Memory Traffic]
```

#### 2.2 Operational Contexts

- High-Frequency Trading (HFT) gateways communicating with matching engines on the same physical host.
- Intra-process thread communication (lock-free actor queues, worker thread pools).
- High-bandwidth video frame or machine learning tensor pipelines where crash recovery is managed by external process supervisors (e.g., systemd, Kubernetes).
- Message payloads exceeding 4,095 bytes (supports full 32-bit `slot->len` up to 4 GiB).

#### 2.2 Paired API Functions & Features

- **Zero-Copy Single-Slot I/O**: `rbipc_reserve_write()`, `rbipc_commit_write()`, `rbipc_read_acquire()`, `rbipc_read_release()`.
- **Dedicated Core Polling**: `RBIPC_WAIT_ZERO_SPIN` or `RBIPC_WAIT_ADAPTIVE` with CPU core affinity (`pthread_setaffinity_np`).
- **Cache-Bypassing Streaming Stores**: `rbipc_write_stream()`, `rbipc_stream_copy()` (AVX2 `_mm256_stream_si256` avoiding cache pollution on large payloads).
- **HugePage Alignment**: Combine with `RBIPC_FLAG_HUGETLB` for buffers $\ge 4\text{ MB}$.

#### 2.2 Anti-Patterns & Caveats

- **Untrusted Peers**: If a producer or consumer process terminates unexpectedly while holding a slot, the slot sequence will remain locked, blocking the queue without automated healing.
- **Payloads $\le 4\text{ KiB}$ in High-Rate Batches**: Prefer Combined Tier 2+3 for small payloads.

---

### 2.3 Tier 3: Packed Descriptor Mode (`RBIPC_FLAG_PACKED`)

#### 2.3 Core Microarchitectural Mechanism

Tier 3 unifies the monotonic sequence turn counter, message payload length, and slot lifecycle state into a single 64-bit integer (`uint64_t`).

```txt
+------------------------------------------------+--------------+-------+
| Bits [63:16] (48 bits)                         | Bits [15:4]  | [3:0] |
| Monotonic Sequence Turn Counter                | Length (12b) | State |
+------------------------------------------------+--------------+-------+
 63                                            16 15           4 3     0
```

1. **Hardware-Atomic 64-Bit Publication**: Committing a slot writes the sequence, length, and state in a **single atomic 64-bit store instruction** (`MOV` on x86-64, `STLR` on ARM64).
2. **Tear-Free Multi-Subscriber Multicast**: In 1-to-N Multicast / PubSub, concurrent subscribers read sequence, length, and state in a **single 64-bit atomic load**, making multi-word torn reads physically impossible without locks or fences.
3. **Shifted SIMD Vector Turn Scanning**: 256-bit AVX2 vector instructions evaluate 4 slots simultaneously via `_mm256_srli_epi64` and `_mm256_cmpeq_epi64`, achieving **514.70 Mslots/s** inspection rates.
4. **Constraint**: Constrains payload lengths to $0 \le \text{len} \le 4{,}095$ bytes (`RBIPC_PACKED_MAX_LEN`).

#### 2.3 Operational Contexts

- 1-to-N Multicast Publish-Subscribe topic feeds (market data dissemination, telemetry broadcasting).
- IoT and telemetry metrics aggregation pipelines.
- Small financial RPC commands and tick distribution ($\le 4\text{ KiB}$).

#### 2.3 Paired API Functions & Features

- **Multicast Publish-Subscribe**: `rbipc_publish()`, `rbipc_publish_batch()`, `rbipc_sub_poll()`, `rbipc_sub_poll_batch()`.
- **B-Queue Vector Batching**: `rbipc_reserve_write_batch()`, `rbipc_commit_write_batch()`.
- **SIMD Hardware Scanning**: Evaluated automatically via internal AVX2/AVX-512 engines.

#### 2.3 Anti-Patterns & Caveats

- **Payloads > 4,095 Bytes**: Passing `slot_size > 4095` to `rbipc_create_ex` returns `RBIPC_ERR_INVAL`.
- **Pure Scalar Spin-Loops without Batching**: Re-evaluating `seq >> 16` inside an un-batched scalar spinloop adds 1 cycle of ALU dependency compared to Tier 2.

---

### 2.4 Combined Tier 2+3: Trusted & Packed Mode (`RBIPC_FLAG_TRUSTED | RBIPC_FLAG_PACKED`)

#### 2.4 Core Microarchitectural Mechanism

Combined Tier 2+3 represents the pinnacle of small-message IPC performance ($\le 4,095$ bytes). It fuses Tier 2's zero-metadata overhead with Tier 3's 64-bit composite descriptor:

1. **Commit Fast-Path**: Exactly **1 atomic store** (`slot->sequence = packed`).
2. **Release Fast-Path**: Exactly **1 atomic store** (`slot->sequence = next_packed`).
3. **Acquisition Fast-Path**: Zero redundant memory loads. The sequence loaded during the polling check is passed directly in a CPU register (`%rax`/`%rdx`) to extract payload length.
4. **Cache Traffic**: 1 store on producer, 1 store on consumer (**2 total stores per round-trip**).

#### 2.4 Operational Contexts

- Ultra-high throughput small-message pipelines ($\le 4\text{ KiB}$) requiring tens of millions of messages/second.
- Financial exchange matching engines and order book dissemination.
- High-rate packet processing (Vector Packet Processing / DPDK-adjacent pipelines).

#### 2.4 Paired API Functions & Features

- **B-Queue Batching**: `rbipc_reserve_write_batch()`, `rbipc_commit_write_batch()` (Batch sizes 16, 32, 64 achieve **35.34 Mops/s** and **28.3 ns latency**).
- **Core Pinning**: Isolate producer and consumer threads on dedicated CPU cores (`sched_setaffinity`).

---

## 3. Memory Backing & Wait Policy Dimensions

### 3.1 Shared Memory Backing

| Backing Mode | Creation Flag | Mechanism | Best Paired With | Operational Context |
| :--- | :--- | :--- | :--- | :--- |
| **Anonymous memfd** (*Default*) | `RBIPC_SHM_ANON` | Linux `memfd_create`, sealed with `F_ADD_SEALS`, zero `/dev/shm` footprint. | `rbipc_send_fd()`, `rbipc_recv_fd_ex()` | Containerized workloads, sandboxed microservices, secure IPC crossing parent-child or socket connections. |
| **Named POSIX SHM** | `RBIPC_SHM_NAMED` | Global `/dev/shm` filesystem object (`shm_open`). | `rbipc_create()`, `rbipc_attach()`, `bin/rbipc_stat` | Independent daemon architectures, persistent IPC rendezvous without pre-existing socket connections. |
| **HugeTLB & Dynamic 2MB THP** | `RBIPC_FLAG_HUGETLB` | 2MB HugeTLB with dynamic Transparent HugePage fallback. | Rings $\ge 4\text{ MB}$, `rbipc_write_stream()` | High-bandwidth streaming buffers (4 MB to 32 MB+). Cuts TLB misses, yielding up to **4.51x speedup (+77.8% latency reduction)**. |

### 3.2 Wait & Backoff Synchronization Policies

| Policy | Configuration Flag | Backoff Ladder | Idle CPU | Best Paired With |
| :--- | :--- | :--- | :--- | :--- |
| **Adaptive Hybrid** (*Default*) | `RBIPC_WAIT_ADAPTIVE` | 48-iteration micro-pause $\to$ 5 µs C0.1 wait $\to$ `sched_yield` $\to$ `SYS_futex` sleep. | 0.13% | General production multi-threaded workloads with mixed burst and idle periods. |
| **Zero-Spin** | `RBIPC_WAIT_ZERO_SPIN` | Immediate passive kernel `futex` sleep on contention. | **0.13% - 0.3%** | Quiescent background daemons, battery-powered devices, power-constrained edge nodes. |
| **Hardware Monitor** | `RBIPC_WAIT_HW_MONITOR` | Intel WAITPKG `UMWAIT` (C0.2) or ARM64 FEAT_WFxT `WFET`. | Low Power | Pinned latency-sensitive threads requiring 40–70 ns wakeups without consuming 100% core TDP. |

---

## 4. Exhaustive Context Matrix & Feature Pairing Directory

| Architecture / Workload Pattern | Recommended Mode Combination | Key Companion Functions | Empirical Telemetry | Rationale & Architectural Synergy |
| :--- | :--- | :--- | :--- | :--- |
| **Multi-Tenant Microservices across Security Boundaries** | **Tier 1 Zero-Trust** + `RBIPC_SHM_ANON` + `RBIPC_WAIT_ADAPTIVE` | `rbipc_create_anon`, `rbipc_send_fd`, `rbipc_set_lease_timeout`, `rbipc_revoke_expired_leases`, `rbipc_abort_write` | 10.65M msgs/s streaming;<br>30.38M msgs/s batch-32 | Fileless anonymous memory prevents discovery attacks; `pidfd_open` audits dead peers without PID recycling hazards; lease timeouts prevent consumer deadlocks. |
| **HFT Gateway $\to$ Matching Engine (Tightly Coupled)** | **Tier 2 Trusted** + `RBIPC_SHM_NAMED` + `RBIPC_WAIT_ZERO_SPIN` | `rbipc_reserve_write`, `rbipc_commit_write`, `rbipc_read_acquire`, `rbipc_read_release`, CPU pinning | **15.16M msgs/s**;<br>**97 ns** p50 latency | Bypasses PID and lease checks entirely. Outperforms DPDK `rte_ring` (8.66M) and LMAX Disruptor (9.89M) in 1P-1C streaming. |
| **Lock-Free 1-to-N Market Data / Telemetry Broadcast** | **Tier 3 Packed** + Multicast PubSub + Named/Anon SHM | `rbipc_publish`, `rbipc_publish_batch`, `rbipc_sub_create_ex`, `rbipc_sub_poll_batch` | **5.07M msgs/s** zero-copy;<br>+82.9% speedup vs std ring | Single 64-bit atomic load guarantees tear-free multicast reads across concurrent lock-free subscribers. |
| **Peak Small-Payload Batch Ingestion ($\le 4\text{ KiB}$)** | **Combined Tier 2+3** + Pinned CPU Cores | `rbipc_reserve_write_batch`, `rbipc_commit_write_batch`, `rbipc_read_acquire_batch`, `rbipc_read_release_batch` | **35.34M msgs/s** (2,156 MB/s);<br>**28.3 ns** latency | 1 store on commit, 1 on release. Out-of-order execution engine unrolls slot unpacking across execution ports, completely hiding bit-shift overhead. |
| **Bulk Zero-Copy Streaming / Video / ML Tensors ($> 4\text{ KiB}$)** | **Tier 2** + `RBIPC_FLAG_HUGETLB` + `RBIPC_WAIT_ADAPTIVE` | `rbipc_write_stream`, `rbipc_stream_copy`, Option B 2MB THP alignment | **19.87 GB/s** physical copy;<br>4.51x speedup on 32MB rings | Unlocks full 32-bit length fields, avoids TLB miss stalls on multi-megabyte buffers, and utilizes AVX2 non-temporal stores to avoid cache pollution. |
| **Asynchronous Event-Driven Services (`epoll` / `io_uring`)** | Any Tier + Reactor Integration | `rbipc_enable_eventfd`, `rbipc_eventfd_drain`, `epoll_ctl` | 874.8k events/s;<br>1.14 µs/event | Binds kernel `eventfd` to shared memory ring, enabling seamless multiplexing into asynchronous event loops. |
| **Battery / Edge / Power-Constrained Devices** | Any Tier + `RBIPC_WAIT_ZERO_SPIN` | `rbipc_set_wait_policy`, `rbipc_read_acquire_timeout`, `bin/rbipc_stat --watch` | **0.13% - 0.3%** idle CPU | Bypasses busy-spinning, putting threads into kernel `futex` sleep immediately when the queue is empty. |

---

## 5. Concrete C23 Implementation Blueprints

### Blueprint A: Multi-Tenant Secure IPC with Leases & Crash Recovery (Tier 1)

```c
#include "rbipc.h"
#include <unistd.h>

void run_producer(int socket_fd) {
    rbipc_ring_t *ring = NULL;
    /* Tier 1 (flags = 0), Anonymous memfd shared memory */
    int rc = rbipc_create_ex(NULL, 1024, 256, RBIPC_SHM_ANON | RBIPC_ROLE_PRODUCER, 0600, &ring);
    if (rc != RBIPC_OK) return;

    /* Enforce 50 ms consumer lease */
    rbipc_set_lease_timeout(ring, 50000000ULL);

    /* Transmit file descriptor across UNIX domain socket to untrusted consumer */
    rbipc_send_fd(socket_fd, rbipc_get_fd(ring));

    void *buf = NULL;
    uint64_t ticket = 0;
    if (rbipc_reserve_write(ring, 128, &buf, &ticket) == RBIPC_OK) {
        /* Populate payload... */
        rbipc_commit_write(ring, ticket, 128);
    }
    rbipc_detach(ring);
}
```

### Blueprint B: Sub-100ns Ultra-Low Latency Order Matching (Tier 2)

```c
#include "rbipc.h"

void run_hft_producer(const char *ring_name) {
    rbipc_ring_t *ring = NULL;
    /* Tier 2 Trusted Mode: pure Vyukov sequence turns */
    int rc = rbipc_create_ex(ring_name, 4096, 64, 
                             RBIPC_SHM_NAMED | RBIPC_FLAG_TRUSTED | RBIPC_ROLE_PRODUCER, 
                             0660, &ring);
    if (rc != RBIPC_OK) return;

    /* Micro-pause spin for dedicated pinned core */
    rbipc_set_wait_policy(ring, RBIPC_WAIT_ZERO_SPIN);

    for (uint64_t i = 0; i < 1000000; ++i) {
        void *buf = NULL;
        uint64_t ticket = 0;
        while (rbipc_reserve_write_nonblock(ring, sizeof(uint64_t), &buf, &ticket) != RBIPC_OK) {
            __builtin_ia32_pause();
        }
        *(uint64_t *)buf = i;
        rbipc_commit_write(ring, ticket, sizeof(uint64_t));
    }
    rbipc_detach(ring);
}
```

### Blueprint C: 1-to-N Multicast Market Data Dissemination (Tier 3 Packed)

```c
#include "rbipc_pubsub.h"

void run_market_data_publisher(const char *topic) {
    rbipc_ring_t *ring = NULL;
    /* Tier 3 Packed Mode: 64-bit atomic composite descriptors */
    rbipc_create_ex(topic, 4096, 128, RBIPC_SHM_NAMED | RBIPC_FLAG_PACKED, 0660, &ring);

    for (uint64_t seq = 0; seq < 500000; ++seq) {
        char tick[64] = "AAPL 150.25 100";
        /* Hardware-atomic publication without torn reads across subscribers */
        rbipc_publish(ring, tick, sizeof(tick));
    }
    rbipc_detach(ring);
}

void run_subscriber(const char *topic) {
    rbipc_ring_t *ring = NULL;
    rbipc_attach(topic, &ring);

    rbipc_sub_t *sub = NULL;
    rbipc_sub_create_ex(ring, false, &sub); /* Stream from latest live head */

    const void *buf = NULL;
    uint32_t len = 0;
    uint64_t seq = 0;
    while (true) {
        if (rbipc_sub_poll(sub, &buf, &len, &seq) == RBIPC_OK) {
            /* Process tick safely with single-load atomic guarantee */
        }
    }
    rbipc_sub_destroy(sub);
    rbipc_detach(ring);
}
```

### Blueprint D: Maximum Throughput Batch Ingestion (Combined Tier 2+3)

```c
#include "rbipc.h"

void run_vector_batch_pipeline(const char *shm_name) {
    rbipc_ring_t *ring = NULL;
    /* Combined Tier 2+3: Trusted & Packed small-message mode */
    rbipc_create_ex(shm_name, 4096, 64, 
                    RBIPC_SHM_NAMED | RBIPC_FLAG_TRUSTED | RBIPC_FLAG_PACKED, 
                    0660, &ring);

    rbipc_iovec_t iovecs[32];
    uint64_t tickets[32];
    uint32_t lens[32];
    uint32_t reserved = 0;

    /* Reserve 32 slots in a single atomic step */
    if (rbipc_reserve_write_batch(ring, 32, iovecs, &reserved) == RBIPC_OK) {
        for (uint32_t i = 0; i < reserved; ++i) {
            tickets[i] = iovecs[i].ticket;
            lens[i] = 64;
            *(uint64_t *)iovecs[i].buf = i;
        }
        /* Commit batch with single memory barrier and hardware-atomic publication */
        rbipc_commit_write_batch(ring, reserved, tickets, lens);
    }
    rbipc_detach(ring);
}
```

---

## 6. Microarchitectural Analysis & Anti-Patterns

### 6.1 Store-Buffer Drain Latency & Cache-Line Dirtying

- Every memory store instruction issued by a CPU core must reside in the hardware store buffer until committed to the L1 data cache.
- In **Tier 1 Zero-Trust**, updating `producer_pid`, `state`, `sequence`, `consumer_pid`, and `timestamp` requires **13 distinct stores**. This creates store-buffer backpressure and generates frequent Read-For-Ownership (RFO) invalidation cycles across core caches via the MESI/MOESI protocol.
- In **Pure Tier 2** and **Combined Tier 2+3**, stores are reduced to **2 or 3 per round trip**, minimizing RFO invalidations and store-buffer stalls.

### 6.2 Instruction-Level Parallelism (ILP) in Packed Processing

- While Tier 3 introduces a logical bit-shift (`seq >> 16`), modern out-of-order execution cores hide this latency when processing **batches**:
  - Independent slot descriptors evaluate simultaneously across CPU execution ports 0, 1, 5, and 6.
  - In batch sizes $\ge 16$, Combined Tier 2+3 achieves **35.34 Mops/s**, outperforming all other modes.

### 6.3 When NOT to Use Each Mode (Anti-Pattern Matrix)

1. **Do NOT use Tier 2 or Tier 2+3** when communicating with untrusted, third-party, or unstable processes that could terminate unexpectedly without releasing slots.
2. **Do NOT use Tier 3 or Tier 2+3** when message payloads exceed 4,095 bytes (use Tier 1 or Tier 2).
3. **Do NOT use Tier 1** in high-frequency trading matching engines or pinned intra-thread pipelines where the latency budget is strictly sub-100 nanoseconds.
4. **Do NOT use HugeTLB (`RBIPC_FLAG_HUGETLB`)** on small rings ($\le 64\text{ KiB}$); the 2MB hugepage allocation overhead provides zero speedup on buffers smaller than the L2 cache.
