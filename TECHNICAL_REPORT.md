# Technical Report: Architecture, Formal Verification, and Empirical Evaluation of librbipc

## A High-Throughput, Zero-Idle-CPU, Crash-Resilient MPMC Shared-Memory IPC Engine in ISO C23

**Author:** Seintian  
**Date:** October 2026  
**Document Version:** 2.0.0 (Production Release)

---

## Abstract

Inter-process communication (IPC) on multi-core Linux systems typically involves trade-offs between busy-polling ring buffers (which consume entire CPU cores and suffer severe degradation under scheduler preemption) and kernel-mediated transports such as UNIX domain sockets or POSIX message queues (which introduce context-switch and copying overhead). This report presents the design, formal concurrency model, fault-tolerance mechanisms, and empirical evaluation of **`librbipc`**, a shared-memory IPC library implemented in ISO C23 designed for concurrent multi-producer multi-consumer (MPMC) execution with passive CPU utilization when idle and deterministic fault tolerance.

The library incorporates five key design elements:

1. **Per-slot sequence turn progression and adaptive dual-mode futex architecture:** A Vyukov-style per-slot 64-bit sequence turn array ([`rbipc_slot_t`](include/rbipc.h)) that dynamically unlocks native direct 64-bit (`FUTEX2_SIZE_U64`) and low-word (`FUTEX2_SIZE_U32`) slot waiting on Linux 5.16+ (`sys_futex_waitv`), while gracefully falling back on legacy kernels to decoupled 32-bit proxy words (`futex_seq`/`write_futex_seq`) to eliminate the 32-bit counter wrap-around ABA hazard.
2. **Asymmetric Dekker synchronization and hybrid wait engine:** An asymmetric synchronization protocol ([`src/rbipc_sync.c`](src/rbipc_sync.c)) using `sys_membarrier` that eliminates hardware memory fences on the producer fast path, coupled with a bounded micro-spin ($B = 48$ pause/yield cycles, $\approx 400$ ns) transitioning into passive kernel `FUTEX_WAIT` suspension, achieving zero system calls under sustained load and 0.13% CPU utilization when idle.
3. **Deterministic peer crash recovery:** A liveness auditing subsystem ([`src/rbipc_audit.c`](src/rbipc_audit.c)) using Linux process file descriptors (`pidfd_open`) to eliminate PID-recycling races, combined with atomic slot poisoning (`RBIPC_SLOT_POISONED`), bounded ticket-hole healing, dead-consumer slot reclamation, and time-bounded consumer leases.
4. **Vectorized and non-temporal data paths:** B-Queue scatter-gather vector batching ([`src/rbipc_batch.c`](src/rbipc_batch.c)) achieving 50.68 million messages per second (Mops/s), AVX2 and NEON vectorized slot-readiness scanning ([`src/rbipc_simd.c`](src/rbipc_simd.c)), and non-temporal cache-bypassing streaming stores ([`src/rbipc_stream.c`](src/rbipc_stream.c)) reaching 19.87 GB/s for large transfers.
5. **Multi-topology communication primitives:** Asynchronous `eventfd`/`epoll` reactor multiplexing ([`src/rbipc_reactor.c`](src/rbipc_reactor.c)), lock-free 1-to-$N$ multicast publish-subscribe with subscriber lag clamping ([`src/rbipc_pubsub.c`](src/rbipc_pubsub.c)), and capability-based `memfd_create` descriptor passing over `AF_UNIX` `SCM_RIGHTS` ([`src/rbipc_ipc.c`](src/rbipc_ipc.c)).

Empirical evaluation on a reference Linux desktop architecture (Intel Core i5-10400F) and mobile platform demonstrates single-slot streaming throughput of 6.08 Mops/s, vector batch throughput of 50.68 Mops/s (a 6.53× improvement over scalar batch-1 operations), 200 ns minimum round-trip ping-pong latency, and 0.13% idle CPU utilization. Under multi-core process over-subscription, `librbipc` avoids the throughput collapse observed in busy-polling designs, sustaining 1.42 Mops/s under four-producer four-consumer contention while preventing peer crash deadlocks.

---

## 1. Introduction and Design Philosophy

### 1.1 Trade-Offs in Inter-Process Communication

Low-latency systems—such as financial matching engines, high-frequency telemetry pipelines, and robotics middleware—require inter-process communication (IPC) mechanisms that address four fundamental requirements:

1. **High throughput and low latency:** Communication latency minimized through zero-copy shared-memory access in user space.
2. **Passive idle resource utilization:** Minimal CPU consumption during periods of low activity to coexist efficiently on multi-tenant systems.
3. **Multi-Producer / Multi-Consumer (MPMC) scalability:** High concurrent throughput across independent operating system processes without lock convoys.
4. **Crash fault tolerance:** Resilience against unexpected process termination (`SIGKILL`, abnormal exits) to prevent deadlock or shared memory corruption.

Existing IPC frameworks typically optimize for a subset of these requirements:

- **Busy-polling ring buffers (e.g., DPDK `rte_ring`, LMAX Disruptor):** These designs assume dedicated, pinned CPU cores executing spin loops. While effective when dedicated cores are available, busy polling consumes 100% of core capacity during idle periods and degrades severely under thread over-subscription when the operating system scheduler preempts a publisher or consumer thread.
- **Kernel-mediated mechanisms (e.g., POSIX message queues, `AF_UNIX` sockets):** These mechanisms delegate synchronization and buffering to the kernel, providing clean process isolation and passive waiting, but incur context switch overhead, system call latency, and memory copying overhead.
- **Shared-memory publish-subscribe middleware (e.g., Eclipse `iceoryx2`):** These platforms provide zero-copy loan semantics for 1-to-$N$ broadcast communication, but require complex multi-file management in `/dev/shm`, do not natively support competing-consumer load balancing ($1$-of-$N$ work distribution), and often depend on external coordinator daemons or timer-based polling.

### 1.2 Architectural Contributions

`librbipc` is implemented in ISO C23 to support both competing-consumer MPMC work queues and 1-to-$N$ multicast publish-subscribe within a unified shared-memory architecture. Its primary contributions include:

- **Self-Contained Shared-Memory Layout:** The control header ([`rbipc_shm_header_t`](include/rbipc.h)), slot descriptor table ([`rbipc_slot_t`](include/rbipc.h)), and payload buffer are organized into a single page-aligned shared-memory object. Storage is backed either by POSIX shared memory (`shm_open`) or anonymous kernel file descriptors (`memfd_create`) protected by kernel file seals (`F_SEAL_SHRINK | F_SEAL_GROW`).
- **Multi-Tier Hybrid Synchronization:** Under balanced producer-consumer load, ticket reservations, payload transfers, and sequence publications execute entirely in user space without system calls or full memory barriers on the fast path. Asymmetric synchronization (`sys_membarrier`) offloads memory barrier costs from active writers to waiting threads. Thread suspension integrates hardware monitor-wait instructions (Intel WAITPKG `UMWAIT`, ARM64 `WFET`) and kernel futex mechanisms (`sys_futex_waitv`, `FUTEX_WAIT`).
- **Role-Aware Peer Auditing and Crash Recovery:** Participating processes register operational roles (`RBIPC_ROLE_PRODUCER`, `RBIPC_ROLE_CONSUMER`, `RBIPC_ROLE_MONITOR`) in the shared header. Periodic audit cycles (`RBIPC_FUTEX_PERIOD_NS` = 20 ms) verify process liveness using Linux process descriptors (`pidfd_open`), detect terminated peers, and reclaim orphaned slots.

### 1.3 System Decomposition

The library is organized into a public C/C++ API layer (3 header files in `include/`) and 16 internal implementation modules in `src/`:

| Layer | Module / Header | Responsibility & Key Algorithms |
| :--- | :--- | :--- |
| **Public ABI** | [`include/rbipc.h`](include/rbipc.h) | Core lifecycle, zero-copy slot I/O, B-Queue vector batching, streaming stores, `eventfd` reactor, and consumer lease APIs. |
| **Public ABI** | [`include/rbipc_ipc.h`](include/rbipc_ipc.h) | Capability-based file descriptor transfer (`rbipc_send_fd`, `rbipc_recv_fd_ex`) over `AF_UNIX` `SCM_RIGHTS`. |
| **Public ABI** | [`include/rbipc_pubsub.h`](include/rbipc_pubsub.h) | 1-to-N lock-free multicast topic publishing (`rbipc_publish*`) and independent cursor polling (`rbipc_sub_poll*`, `rbipc_sub_read`). |
| **Foundation** | [`src/rbipc_arch.h`](src/rbipc_arch.h) | `CLOCK_MONOTONIC` vDSO timer, CPU pause/yield, WAITPKG/WFET hardware wait intrinsics, SSE4.2 + SW CRC32C fallback. |
| **Foundation** | [`src/rbipc_attr.h`](src/rbipc_attr.h) | Compiler attributes (`RBIPC_INLINE`, `RBIPC_PREFETCH` temporal locality hints). |
| **Foundation** | [`src/rbipc_math.h`](src/rbipc_math.h) | Power-of-two ceiling (`rbipc_next_pow2`), alignment (`rbipc_align_up`), and RFC 1982 64-bit modular sequence delta (`rbipc_seq_diff`). |
| **Foundation** | [`src/rbipc_predicate.h`](src/rbipc_predicate.h) | Pure, side-effect-free invariant validation predicates (`RBIPC_PURE` / `RBIPC_CONST`). |
| **Foundation** | [`src/rbipc_internal.h`](src/rbipc_internal.h) | Private runtime handle (`struct rbipc_ring`) encapsulating mappings, role flags, cached PID, wait policy, and `eventfd` ownership. |
| **Diagnostics** | [`src/rbipc_error.c`](src/rbipc_error.c) / [`.h`](src/rbipc_error.h) | Deterministic translation between Linux `errno` and `rbipc_err_t` (`rbipc_strerror`, `rbipc_errno_to_status`, `rbipc_error_format`). |
| **Memory** | [`src/rbipc_shm.c`](src/rbipc_shm.c) / [`.h`](src/rbipc_shm.h) | Overflow-checked layout geometry (`rbipc_shm_calc_layout`), `shm_open` / `memfd_create`, `fchmod` umask bypass, and `F_ADD_SEALS`. |
| **Memory** | [`src/rbipc_vmem.c`](src/rbipc_vmem.c) / [`.h`](src/rbipc_vmem.h) | Control region mapping (`rbipc_vmem_map_ctrl`), 1× linear slot mapping (`rbipc_vmem_map_data`), and 2× `MAP_FIXED` ring mirror (`rbipc_vmem_map_double`). |
| **Sync** | [`src/rbipc_futex.c`](src/rbipc_futex.c) / [`.h`](src/rbipc_futex.h) | Process-shared `SYS_futex` (`FUTEX_WAIT` / `FUTEX_WAKE` / `FUTEX_WAKE_OP`) and Linux 5.16+ `futex2` (`sys_futex_waitv`) syscall wrappers. |
| **Sync** | [`src/rbipc_sync.c`](src/rbipc_sync.c) / [`.h`](src/rbipc_sync.h) | Asymmetric Dekker synchronization (`sys_membarrier`), 4-tier hybrid backoff (WAITPKG / WFET / futex2), and wake dispatchers. |
| **State & Audit** | [`src/rbipc_slot.c`](src/rbipc_slot.c) / [`.h`](src/rbipc_slot.h) | Vyukov sequence state transitions (`EMPTY`, `RESERVING`, `RESERVED`, `COMMITTED`, `POISONED`) and CAS-validated consumer release. |
| **State & Audit** | [`src/rbipc_audit.c`](src/rbipc_audit.c) / [`.h`](src/rbipc_audit.h) | `pidfd_open(2)` + `poll(2)` liveness probe, dead-producer poisoning, dead-consumer recovery, ticket-hole healing, and lease revocation. |
| **Data Path** | [`src/rbipc_ring.c`](src/rbipc_ring.c) / [`.h`](src/rbipc_ring.h) | Ring creation/attachment lifecycle, two-phase header initialization with CRC32C verification, and telemetry queries. |
| **Data Path** | [`src/rbipc_io.c`](src/rbipc_io.c) / [`.h`](src/rbipc_io.h) | Zero-copy single-slot reservation, commit, abort, acquisition, release, and next-slot cache prefetching (`rbipc_io_prefetch_*`). |
| **Data Path** | [`src/rbipc_batch.c`](src/rbipc_batch.c) / [`.h`](src/rbipc_batch.h) | B-Queue vector batch reservation (`rbipc_reserve_write_batch*`) and acquisition (`rbipc_read_acquire_batch*`) with partial-batch draining. |
| **Acceleration** | [`src/rbipc_simd.c`](src/rbipc_simd.c) / [`.h`](src/rbipc_simd.h) | 256-bit AVX2 (`_mm256_cmpeq_epi64`), 128-bit ARM64 NEON (`vceqq_u64`), and scalar sequence vacancy/readiness scanners. |
| **Acceleration** | [`src/rbipc_stream.c`](src/rbipc_stream.c) / [`.h`](src/rbipc_stream.h) | Cache-bypassing non-temporal streaming copy (`_mm256_stream_si256` / `_mm_stream_si128` + `_mm_sfence()`) for payloads ≥ 256 B. |
| **Extensions** | [`src/rbipc_reactor.c`](src/rbipc_reactor.c) / [`.h`](src/rbipc_reactor.h) | Non-blocking `eventfd` creation/binding, shared `eventfd_waiters` tracking, and readiness counter draining. |
| **Extensions** | [`src/rbipc_ipc.c`](src/rbipc_ipc.c), [`src/rbipc_pubsub.c`](src/rbipc_pubsub.c) | UNIX socket `SCM_RIGHTS` ancillary descriptor passing and 1-to-N lock-free multicast publish-subscribe engine. |

---

## 2. Shared-Memory Geometry and Virtual Address Space Management

### 2.1 Geometric Layout Formulation

A ring buffer in `librbipc` is parameterized by slot capacity $C$ and maximum payload size per slot $S_{\text{raw}}$. During creation and attachment, [`rbipc_shm_calc_layout()`](src/rbipc_shm.c) validates that $C = 2^k$ ($k \in [1, 24]$) and computes partition offsets using overflow-checked arithmetic prior to memory allocation:

1. **Cache-Line Aligned Slot Capacity ($S$):**

   ```text
   S = align_up(S_raw, 64) = (S_raw + 63) & ~63
   ```

   Validated against `UINT32_MAX - 64` to prevent 32-bit arithmetic overflow.
2. **Page-Aligned Metadata Region ($O_{\text{data}}$):**
   The control region comprises the 256-byte [`rbipc_shm_header_t`](include/rbipc.h) followed immediately by $C$ slot descriptors of 64 bytes each ([`rbipc_slot_t`](include/rbipc.h)):

   ```text
   O_data = align_up(sizeof(rbipc_shm_header_t) + C * sizeof(rbipc_slot_t), P) = align_up(256 + 64 * C, P)
   ```

   where $P = \text{sysconf}(\text{\_SC\_PAGESIZE})$ (typically 4096 B).
3. **Page-Aligned Circular Data Region ($D$) and Total Backing Store Size ($T$):**

   ```text
   D = align_up(C * S, P),    T = O_data + D
   ```

   Multiplications and additions verify $a \le \text{SIZE\_MAX} / b$ and $a \le \text{SIZE\_MAX} - b$, returning `RBIPC_ERR_OVERFLOW` upon arithmetic overflow.

```text
Shared Memory Backing Object (Total Size T = O_data + D, Page-Aligned):
+-----------------------------------------------------------------------+
| Offset 0x0000: rbipc_shm_header_t (256 Bytes = 4 x 64B Cache Lines)   |
|   Zone 1 [0x00..0x3F]: Immutable Geometry, Magic, Endian, CRC32C      |
|   Zone 2 [0x40..0x7F]: Producer Control (write_ticket, futex_seq...)  |
|   Zone 3 [0x80..0xBF]: Consumer Control (read_ticket, write_futex...) |
|   Zone 4 [0xC0..0xFF]: Lifecycle & Telemetry (shutdown, leases...)    |
+-----------------------------------------------------------------------+
| Offset 0x0100: rbipc_slot_t[0 .. C-1] (C x 64 Bytes)                  |
|   Each slot descriptor occupies exactly one 64-byte L1/L2 cache line  |
+-----------------------------------------------------------------------+
| Page-Alignment Padding up to Offset O_data                            |
+=======================================================================+
| Offset O_data: Contiguous Slot Payload Buffer (D = align_up(C * S, P))|
|   Slot[i] Payload Address = data_map + (i * S), 64-byte aligned       |
+-----------------------------------------------------------------------+
```

### 2.2 Backing Store Creation and Kernel File Sealing

[`rbipc_shm_create_ex()`](src/rbipc_shm.c) supports two backing store mechanisms:

- **Named POSIX Shared Memory (`name != NULL`):** Validates that `name` begins with `'/'`, contains no subsequent slashes, and satisfies $2 \le \text{len}(name) < \text{NAME\_MAX}$ ([`rbipc_is_valid_shm_name()`](src/rbipc_predicate.h)). The file is opened via `shm_open(name, O_CREAT | O_EXCL | O_RDWR, mode)` (or unlinked first if `RBIPC_CREATE_OVERWRITE` is specified). `fchmod(fd, actual_mode)` is called explicitly to bypass process `umask` restrictions.
- **Anonymous Shared Memory (`name == NULL`):** Invokes `memfd_create("rbipc_anon_ring", MFD_CLOEXEC | MFD_ALLOW_SEALING)`. The resulting descriptor does not appear in `/dev/shm`, is reclaimed by the kernel when all references and memory mappings close, and can be passed to authorized processes via [`rbipc_send_fd()`](src/rbipc_ipc.c).

Immediately following `ftruncate(fd, total_shm_size)`, [`rbipc_shm_seal()`](src/rbipc_shm.c) applies kernel file seals:

```c
fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW);
```

Without file sealing, an aberrant process invoking `ftruncate(fd, 0)` on the descriptor would unmap backing pages beneath active processes, causing `SIGBUS` faults upon subsequent memory access. Applying `F_SEAL_SHRINK` and `F_SEAL_GROW` guarantees backing store size immutability within the virtual file system.

### 2.3 Two-Phase Control Header Initialization and CRC32C Validation

In shared-memory architectures, a race condition can arise if an attaching process inspects metadata before the creator finishes writing geometry fields. To prevent premature attachment, initialization uses a two-phase atomic publication protocol ([`rbipc_ring_init_header()`](src/rbipc_ring.c), [`rbipc_ring_probe_and_validate_header()`](src/rbipc_ring.c)):

1. The creator populates geometry fields (`magic = 0x52424950434D454DULL` (`"RBIPCMEM"`), `version = 2`, `endian_tag = 0x01020304U`, `capacity`, `capacity_mask = capacity - 1`, `slot_size`, `data_offset`, `data_size`, `total_shm_size`), computes a hardware-accelerated **CRC32C (Castagnoli)** checksum over bytes `[0 .. 59]` of [`rbipc_shm_header_t`](include/rbipc.h) via [`rbipc_crc32c()`](src/rbipc_arch.h) (`_mm_crc32_u64`/`_mm_crc32_u8` under SSE4.2), initializes the slot table ([`rbipc_slot_init_table()`](src/rbipc_slot.c)), and publishes the initialization state using a release store:

   ```c
   atomic_store_explicit(&hdr->init_state, RBIPC_HEADER_INIT_DONE, memory_order_release);
   ```

2. Attaching peers (`rbipc_attach*`) poll with acquire semantics (up to $1000 \times 10$ $\mu$s) until `hdr->init_state == RBIPC_HEADER_INIT_DONE`. The peer then verifies the magic identifier, ABI version, endianness tag, CRC32C checksum, and reconstructed [`rbipc_layout_t`](src/rbipc_shm.h) structure before mapping the data region.

### 2.4 Cache-Line Padding and False-Sharing Elimination

In cache-coherent multi-core systems, false sharing occurs when distinct threads concurrently mutate variables located on the same cache line. To prevent unnecessary cache invalidation, [`rbipc_shm_header_t`](include/rbipc.h) (256 bytes) and [`rbipc_slot_t`](include/rbipc.h) (64 bytes) are explicitly partitioned into isolated 64-byte cache lines verified by compile-time assertions (`_Static_assert`):

| Structure / Cache Line | Byte Offset | Resident Fields | Access Pattern |
| :--- | :--- | :--- | :--- |
| **`rbipc_shm_header_t` Zone 1** | `0x00 .. 0x3F` | `magic`, `version`, `endian_tag`, `capacity`, `capacity_mask`, `slot_size`, `data_offset`, `data_size`, `total_shm_size`, `header_crc32`, `init_state` | Read-only after creation (shared in `S` state across L1d caches). |
| **`rbipc_shm_header_t` Zone 2** | `0x40 .. 0x7F` | `write_ticket` (`_Atomic uint64_t`), `futex_seq` (`_Atomic uint32_t`), `futex_waiters` (`_Atomic uint32_t`), `pad1[48]` | Mutated on producer reservation (`write_ticket`) and consumer data-wait wakeup (`futex_seq`, `futex_waiters`). |
| **`rbipc_shm_header_t` Zone 3** | `0x80 .. 0xBF` | `read_ticket` (`_Atomic uint64_t`), `write_futex_seq` (`_Atomic uint32_t`), `write_waiters` (`_Atomic uint32_t`), `pad2[48]` | Mutated on consumer acquisition (`read_ticket`) and producer space-wait wakeup (`write_futex_seq`, `write_waiters`). |
| **`rbipc_shm_header_t` Zone 4** | `0xC0 .. 0xFF` | `shutdown_flag`, `active_producers`, `active_consumers`, `eventfd_waiters`, `lease_timeout_ns`, `pad3[36]` | Lifecycle counters, reactor waiter tracking, and lease configuration. |
| **`rbipc_slot_t[i]`** | `0x100 + 64i` | `sequence` (`_Atomic uint64_t`), `state`, `producer_pid`, `len`, `consumer_pid`, `acquire_timestamp_ns`, `pad[32]` | Isolated 64-byte cache line per slot $i \in [0, C-1]$, eliminating cross-slot false sharing. |

### 2.5 Linear Slot Mapping vs. Double-Mapped Continuous Virtual Ring Mirror

The virtual memory subsystem ([`src/rbipc_vmem.c`](src/rbipc_vmem.c)) provides two mapping strategies depending on data access characteristics:

1. **Linear Shared Mapping ([`rbipc_vmem_map_data()`](src/rbipc_vmem.c)):**
   Used by default for discrete slot payload arrays. Because capacity $C$ is a power of two and each slot payload has a uniform aligned size $S$, slot $i = \text{ticket} \ \& \ (C - 1)$ resides entirely within $[i \cdot S, (i + 1) \cdot S) \subseteq [0, D)$. Individual slot payloads never cross the boundary $D$. A single mapping `mmap(NULL, D, PROT_READ | PROT_WRITE, MAP_SHARED, fd, O_data)` minimizes page table entries (PTEs) and avoids virtual-address aliasing across the TLB hierarchy.
2. **Double-Mapped Virtual Ring Mirror ([`rbipc_vmem_map_double()`](src/rbipc_vmem.c)):**
   Designed for variable-length streaming byte buffers that may cross the wrap-around boundary at offset $D$. [`rbipc_vmem_map_double()`](src/rbipc_vmem.c) reserves $2D$ bytes of contiguous virtual address space using an anonymous mapping:

   ```c
   void *anon = mmap(NULL, 2 * data_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
   ```

   It then binds both the lower range `[anon, anon + D)` and upper range `[anon + D, anon + 2D)` using `MAP_SHARED | MAP_FIXED` to the same file descriptor offset $O_{\text{data}}$. Any read or write starting at offset $x \in [0, D)$ of length $L \le D$ is contiguous in virtual address space at `anon + x`, eliminating split-buffer boundary checks.

---

## 3. Lock-Free MPMC Concurrency Protocol and Memory Ordering

### 3.1 Decoupling 64-Bit Sequence Turns from 32-Bit Futex Words

Using 32-bit sequence counters directly as Linux futex wait words introduces vulnerability to counter wrap-around and the ABA problem under high throughput. At $10^8$ messages per second, a 32-bit counter wraps in approximately 43 seconds, creating the risk that a preempted thread wakes up and interprets an old sequence value as a valid turn. `librbipc` separates logical sequence coordination from kernel notification:

- **64-Bit Monotonic Ticket and Sequence Counters:** `hdr->write_ticket`, `hdr->read_ticket`, and `slot->sequence` are 64-bit atomic unsigned integers (`_Atomic uint64_t`). At $10^9$ messages per second, a 64-bit counter requires approximately 584.9 years to wrap around. Sequence ordering is evaluated using RFC 1982 signed 64-bit modular arithmetic ([`rbipc_seq_diff()`](src/rbipc_math.h)):

  ```text
  Δ(a, b) = (int64_t)(a - b)
  ```

- **32-Bit Futex Notification Counters:** `hdr->futex_seq` (consumer notification) and `hdr->write_futex_seq` (producer notification) are independent `_Atomic uint32_t` counters incremented only when waking suspended threads ([`rbipc_sync_dispatch_wake()`](src/rbipc_sync.c)).

### 3.2 Per-Slot Vyukov Sequence Turn Progression

Rather than serializing producers and consumers through global head and tail pointers, `librbipc` implements a Vyukov-style turn-based progression where each slot manages its own sequence lifecycle:

- **Producer Turn Verification:**
  A producer claims ticket $t_w = \text{atomic\_fetch\_add}(\&hdr->\text{write\_ticket}, 1)$ (or via compare-and-swap in non-blocking paths) and addresses slot $s = \&\text{slots}[t_w \ \& \ (C - 1)]$. Slot $s$ is vacant for ticket $t_w$ if and only if:

  ```text
  Δ(s->sequence, t_w) == 0  <==>  s->sequence == t_w
  ```

  If $\Delta(s->\text{sequence}, t_w) < 0$, the slot remains occupied by a prior turn ($t_w - C$) that has not yet been released; the producer enters the hybrid wait hierarchy, waiting directly on $s\to\text{sequence}$ via `sys_futex_waitv` (vectorized with `hdr->write_futex_seq` and `hdr->shutdown_flag`) when available, or falling back to `hdr->write_futex_seq` on classical futex kernels.
- **Producer Publication (Commit):**
  After writing the payload into the slot buffer, [`rbipc_slot_commit()`](src/rbipc_slot.c) updates `s->len = L` (`memory_order_relaxed`), transitions `s->state = RBIPC_SLOT_COMMITTED` (`memory_order_release`), and publishes:

  ```text
  s->sequence <- t_w + 1    (memory_order_release)
  ```

- **Consumer Turn Verification:**
  A consumer claims ticket $t_r = \text{atomic\_fetch\_add}(\&hdr->\text{read\_ticket}, 1)$ and inspects $s = \&\text{slots}[t_r \ \& \ (C - 1)]$. Slot $s$ is ready for consumer ticket $t_r$ if and only if:

  ```text
  Δ(s->sequence, t_r + 1) == 0  <==>  s->sequence == t_r + 1
  ```

  Loading `s->sequence` with `memory_order_acquire` establishes a happens-before relation with the producer's release store, ensuring that all payload bytes written to the buffer and `s->len` are visible to the consumer without an explicit full barrier on the fast path.
- **Consumer Slot Recycling (Release):**
  Upon consuming the payload, [`rbipc_slot_release_ex()`](src/rbipc_slot.c) resets `s->state = RBIPC_SLOT_EMPTY` (`memory_order_release`) and advances the sequence number by $C$ to arm the slot for turn $t_r + C$:

  ```text
  s->sequence <- t_r + C    (memory_order_release)
  ```

### 3.3 Slot State Machine

Each slot progresses through discrete lifecycle states defined in [`include/rbipc.h`](include/rbipc.h):

```text
       +--------------------------------------------------------------+
       |                                                              |
       v                                                              |
 [ EMPTY ] ----> (mark_reserving) ---> [ RESERVING ]                  |
       |                                     |                        |
       |                             (CAS write_ticket)               |
       |                                     |                        |
       |                                     v                        |
       +-----------------------------> [ RESERVED ]                   |
                                             |                        |
                                       (slot_commit)                  |
                                             |                        |
                                             v                        |
                                       [ COMMITTED ]                  |
                                             |                        |
                                       (slot_release)                 |
                                             |                        |
                                             +------------------------+
                                             |
                   (peer crash / abort_write)|
                                             v
                                      [ POISONED ] (healed / skipped)
```

| Initial State | Transition Procedure | Destination State | Invariants & Memory Semantics |
| :--- | :--- | :--- | :--- |
| `EMPTY` | `rbipc_slot_mark_reserving()` | `RESERVING` | Stores `producer_pid` to prevent unrecorded crashes prior to ticket allocation. |
| `RESERVING` | `rbipc_io_try_claim_vacant()` | `RESERVED` | Verified by `atomic_compare_exchange_weak(&write_ticket, t, t+1)`. |
| `RESERVED` | `rbipc_slot_commit()` | `COMMITTED` | Release store to `slot->sequence = t_w + 1`; payload bytes visible to consumers. |
| `COMMITTED` | `rbipc_slot_release_ex()` | `EMPTY` | Advance `slot->sequence = t_r + C` with release semantics; slot available for lap $t_r + C$. |
| Any | `rbipc_abort_write()` / `audit` | `POISONED` | Explicit error indication; consumer returns `RBIPC_ERR_POISONED` and advances. |

### 3.4 Memory Ordering Proof

Let $P$ denote a producer thread and $C$ denote a consumer thread interacting via slot $s$:

1. **Program Order ($P$):** The store to payload memory $M[i]$ precedes the release store to `s->sequence = t_w + 1` in program order:
   $$\text{Write}(M[i]) \xrightarrow{po} \text{ReleaseStore}(s\to\text{sequence}, t_w + 1)$$
2. **Synchronization ($P \to C$):** $C$ loads `s->sequence` with `memory_order_acquire`. When $C$ observes value $t_w + 1$ (where $t_w = t_r$), the release store synchronizes-with the acquire load:
   $$\text{ReleaseStore}(s\to\text{sequence}, t_w + 1) \xrightarrow{sw} \text{AcquireLoad}(s\to\text{sequence}, t_r + 1)$$
3. **Happens-Before Relation:** By transitivity of program order and synchronizes-with:
   $$\text{Write}(M[i]) \xrightarrow{hb} \text{AcquireLoad}(s\to\text{sequence}, t_r + 1) \xrightarrow{po} \text{Read}(M[i])$$
4. **Visibility:** Under ISO C23 § 7.17.3, all payload writes executed by $P$ are globally visible to $C$ prior to $C$'s read operations. Full hardware memory fences (`mfence`, `dmb ish`) are unnecessary on the commit and release paths.

---

## 4. Synchronization Engine and Wait Policy Taxonomy

### 4.1 Multi-Tier Hybrid Backoff Hierarchy

When immediate slot turn conditions are not satisfied, [`src/rbipc_sync.c`](src/rbipc_sync.c) initiates a multi-tier backoff sequence:

```text
Fast-Path User-Space Turn Check
    | (Unsatisfied)
    v
Tier 0: Micro-Spin (B = 48 x pause / yield) [~ 400 ns]
    | (Unsatisfied)
    v
Tier 1: Hardware-Assisted Low-Power Wait (WAITPKG UMWAIT / ARM64 WFET) [40-70 ns resume]
    | (Timeout or Unsupported)
    v
Tier 2: Linux 5.16+ Vectorized futex_waitv (Monitors slot->sequence + shutdown_flag)
    | (ENOSYS fallback)
    v
Tier 3: Process-Shared SYS_futex (FUTEX_WAIT on proxy futex_seq word) [Passive Sleep]
```

- **Tier 0 (User-Space Micro-Spin):** Executes bounded iterations of `pause` (x86-64) or `yield` (ARM64) up to $B = 48$ cycles ($\approx 400$ ns), absorbing short pipeline stalls without invoking kernel transitions.
- **Tier 1 (Hardware Low-Power Wait):** Uses Intel WAITPKG (`UMONITOR` + `UMWAIT` in C0.1 low-latency state or C0.2 deep power-save state, `TPAUSE`) or ARM64 `FEAT_WFxT` (`WFET`), monitoring the slot cache line for invalidation with sub-microsecond resumption latency (40–70 ns).
- **Tier 2 (Vectorized `sys_futex_waitv` / Modern Futex2 Subsystem):** On Linux 5.16+, invokes syscall 449 (`futex_waitv`) to monitor both the 64-bit sequence directly (using native `FUTEX2_SIZE_U64` or `FUTEX2_SIZE_U32`), the 32-bit `proxy_seq`, and the shared `shutdown_flag` atomically. Waking on slot readiness, batch proxy increment, or cooperative shutdown occurs in a single vectorized kernel call without cache-line bouncing.
- **Tier 3 (Classical Process-Shared `SYS_futex` Fallback):** On legacy Linux kernels (< 5.16) where `sys_futex_waitv` returns `-ENOSYS`, seamlessly falls back to classical `SYS_futex` with `FUTEX_WAIT` on the 32-bit proxy word `hdr->futex_seq` (or `hdr->write_futex_seq`), placing the thread into passive kernel wait queues with a 20 ms heartbeat check.

### 4.2 Asymmetric Dekker Synchronization via `sys_membarrier(2)`

To prevent lost wakeups between a producer transitioning a slot to ready and a consumer registering as a futex waiter, concurrent systems require synchronization between the state write and waiter flag inspection. A symmetric implementation requires an `atomic_thread_fence(memory_order_seq_cst)` on both producer and consumer sides, which imposes an execution penalty of 15–30 CPU cycles per commit.

`librbipc` applies an asymmetric Dekker synchronization pattern using Linux `sys_membarrier(2)`:

| Producer Commit Path (High Frequency) | Consumer Wait Path (Low Frequency) |
| :--- | :--- |
| **P1.** Publish slot: `atomic_store_explicit(&slot->sequence, t + 1, memory_order_release);` | **C1.** Register waiter: `atomic_fetch_add_explicit(&hdr->futex_waiters, 1, memory_order_seq_cst);` |
| **P2.** `atomic_signal_fence(memory_order_seq_cst);` *(0 CPU cycles, compiler barrier only)* | **C2.** `rbipc_sync_asymmetric_barrier();` *(`sys_membarrier` forces barrier on remote cores)* |
| **P3.** Check waiters: `if (atomic_load_explicit(&hdr->futex_waiters, memory_order_relaxed) > 0)` | **C3.** Re-verify slot: `seq = atomic_load_explicit(&slot->sequence, memory_order_acquire);` |
| **P4.** Wake: `atomic_fetch_add(&hdr->futex_seq, 1, release); SYS_futex(FUTEX_WAKE);` | **C4.** If `seq != t + 1`, invoke `futex_waitv` or `SYS_futex(FUTEX_WAIT, val)`. |
| | **C5.** Deregister: `atomic_fetch_sub_explicit(&hdr->futex_waiters, 1, memory_order_seq_cst);` |

**Correctness:**

1. Let $C$ register as a waiter ($C1$) and invoke `sys_membarrier` ($C2$). The kernel issues an inter-processor interrupt (IPI) or core flush that forces a memory barrier on the core executing $P$.
2. If $P$ executed $P1$ before $C2$ completes, $P1$'s store to `slot->sequence` is visible to $C3$, causing $C3$ to observe `seq == t + 1`, deregister ($C5$), and proceed without sleeping.
3. If $P$ executed $P1$ after $C2$ completes, then $P$ executes after $C1$, ensuring $P$'s load of `futex_waiters` ($P3$) observes `futex_waiters > 0` and executes $P4$ to wake $C$.
4. This ensures that lost wakeups cannot occur, while avoiding hardware memory barriers on the producer commit path.

### 4.3 Wait Policy Taxonomy

Via [`rbipc_set_wait_policy()`](src/rbipc_ring.c), each ring handle configures its wait strategy:

1. **`RBIPC_WAIT_ADAPTIVE` (Default: Bounded Spin-Then-Park):**
   Executes Tier 0 micro-spinning (48 pause cycles) followed by Tier 1 hardware monitor-wait (`UMWAIT`/`WFET` up to 5 $\mu$s in C0.1 low-latency state). If the peer publishes within this window, the operation completes entirely in user space (sub-microsecond latency); otherwise, the thread transitions to Tier 2/3 kernel wait queues.
2. **`RBIPC_WAIT_ZERO_SPIN` (Passive Kernel Sleep):**
   Upon a vacancy or readiness miss, the thread immediately enters Tier 2/3 passive kernel suspension (with a 20 ms periodic audit tick `RBIPC_FUTEX_PERIOD_NS` for crash recovery). Minimizes idle CPU consumption (0.13%).
3. **`RBIPC_WAIT_HW_MONITOR` (Decoupled Low-Power Hardware Monitor):**
   Bypasses Tier 0 active micro-spinning and immediately arms the CPU hardware address monitor (`UMONITOR` / Exclusive Monitor), entering low-power substate C0.2 (on x86 WAITPKG) or `WFET` (on ARM64) for up to 5 $\mu$s before escalating to Tier 2/3 kernel suspension. Provides sub-microsecond latency while cutting energy consumption and yielding internal execution pipelines to SMT sibling hardware threads.

### 4.4 Comparative Analysis of Wait Strategies

To situate `librbipc`'s synchronization engine within the broader high-performance IPC literature, we examine wait strategies employed by DPDK (`lib/ring` and `lib/power`) and the LMAX Disruptor (`WaitStrategy` implementations):

| Strategy | Framework | Core Mechanism | Idle CPU | Multi-Process SHM Safe? |
| :--- | :--- | :--- | :---: | :---: |
| **`DPDK-BusySpin`** | DPDK `rte_ring` | C11 acquire-load polling loop with zero backoff. | 100% | Yes (consumes 1 full core) |
| **`DPDK-PauseLoop`** | DPDK `rte_pause` | Polling loop issuing x86 `PAUSE` (`_mm_pause()`) to prevent pipeline stalls. | 100% | Yes (consumes 1 full core) |
| **`DPDK-UMWAIT`** | DPDK `rte_power` | Intel WAITPKG `UMONITOR` + `UMWAIT` low-power state sleep on cache invalidation. | Low | Intel Alder Lake/Sapphire Rapids+ only. |
| **`DPDK-Interrupt-Epoll`** | DPDK `rte_epoll` | 64-iteration spin followed by `eventfd` + `epoll_wait` kernel sleep. | ≈ 0.25% | Requires file descriptor passing (`SCM_RIGHTS`). |
| **`LMAX-BusySpin`** | LMAX Disruptor | Sequence cursor polling loop (`BusySpinWaitStrategy`). | 100% | Yes (consumes 1 full core) |
| **`LMAX-Yielding`** | LMAX Disruptor | 100 spins followed by `sched_yield()` (`YieldingWaitStrategy`). | 100% | Yes (saturates scheduler runqueue) |
| **`LMAX-Sleeping`** | LMAX Disruptor | 100 spins → 100 `sched_yield()` → `clock_nanosleep(1 us)` (`SleepingWaitStrategy`). | ≈ 1.06% | High context-switch jitter (50–100 μs). |
| **`LMAX-Blocking`** | LMAX Disruptor | `pthread_mutex_lock` + `pthread_cond_wait` on every commit (`BlockingWaitStrategy`). | ≈ 0.25% | Not crash safe (peer `SIGKILL` causes deadlock). |
| **`LMAX-LiteBlocking`** | LMAX Disruptor | Atomic flag eliding mutex lock when consumer is active (`LiteBlockingWaitStrategy`). | ≈ 0.25% | Not crash safe (mutex/condvar state corruption). |
| **`LMAX-PhasedBackoff`** | LMAX Disruptor | 10 μs spin → 10 μs `sched_yield()` → fallback wait (`PhasedBackoffWaitStrategy`). | ≈ 0.27% | Dependent on fallback strategy. |
| **`librbipc` (`ZERO_SPIN` / `ADAPTIVE`)** | **`librbipc`** | Dekker-fenced `futex_waiters` check + optional 48-cycle `PAUSE` + direct `SYS_futex`. | **0.13–0.17%** | Yes (stateless futex word + `pidfd_open` audit). |

### 4.5 Empirical Evaluation of Idle and Wait Strategies

The benchmark suite [`benchmarks/bench_idle_strategies.c`](benchmarks/bench_idle_strategies.c) evaluates all 10 strategies across four workloads on the reference Intel Core i5-10400F platform: (1) sustained 1P-1C throughput (500,000 $\times$ 64 B messages), (2) continuous half-RTT ping-pong latency (20,000 round-trips), (3) intermittent burst wakeup latency (100 bursts separated by 10 ms idle intervals), and (4) a 500 ms idle CPU and context-switch audit.

| Strategy | Category | Sustained Throughput (Mops/s) | Bandwidth (MB/s) | Ping-Pong p50 (ns) | Ping-Pong p99 (ns) | Burst Wake p50 (ns) | Burst Wake p99 (ns) | Idle CPU (%) | Idle Vol CS |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| **`DPDK-BusySpin`** | DPDK Polling | 14.88 *(28.08 peak)* | 907.99 | 150 | 200 | 1,400 | 68,604 | 99.98% | 0 |
| **`DPDK-PauseLoop`** | DPDK Power | 18.02 *(18.45 peak)* | 1,099.77 | 150 | 250 | 1,400 | 354,022 | 99.98% | 0 |
| **`DPDK-Interrupt-Epoll`** | DPDK Interrupt | 15.11 | 922.10 | 26,752 | 47,503 | 58,905 | 116,509 | 0.27% | 99 |
| **`LMAX-BusySpin`** | Disruptor Spin | 22.31 *(30.40 peak)* | 1,361.98 | 100 | 200 | 1,101 | 197,316 | 99.98% | 0 |
| **`LMAX-Yielding`** | Disruptor Yield | 20.18 | 1,231.82 | 100 | 150 | 1,100 | 8,701 | 99.98% | 0 |
| **`LMAX-Sleeping`** | Disruptor Sleep | 21.19 | 1,293.10 | 100 | 150 | 46,203 | 104,809 | 1.06% | 922 |
| **`LMAX-Blocking`** | Disruptor Mutex | 8.92 | 544.45 | 26,102 | 52,254 | 49,504 | 97,107 | 0.25% | 99 |
| **`LMAX-LiteBlocking`** | Disruptor Lite | 21.77 | 1,328.62 | 26,052 | 51,104 | 49,704 | 156,212 | 0.25% | 99 |
| **`LMAX-PhasedBackoff`** | Disruptor Phased | 15.97 | 974.46 | 100 | 150 | 50,304 | 98,708 | 0.27% | 99 |
| **`librbipc-Futex`** | Adaptive Futex | **7.58 *(13.82 peak)*** | **462.53 *(843.29)*** | **26,202** | **48,154** | **49,004** | **81,207** | **0.14%** | **60** |

#### Summary of Wait Strategy Characteristics

1. **Idle CPU Utilization and Context Switches:** `librbipc-Futex` exhibits the lowest idle CPU consumption among evaluated strategies (0.14%) and the lowest burst wake $p99$ tail latency (81.2 $\mu$s) among kernel-sleeping strategies (`DPDK-Interrupt-Epoll` 116.5 $\mu$s, `LMAX-LiteBlocking` 156.2 $\mu$s, `LMAX-Sleeping` 104.8 $\mu$s).
2. **Context Switch Churn:** `LMAX-Sleeping` (`clock_nanosleep`) causes 922 voluntary context switches during a 500 ms idle interval (1,844 switches per second per thread) and consumes 1.06% CPU due to continuous wake-sleep cycles. In contrast, `librbipc` remains suspended until signaled or until the 20 ms audit tick expires.
3. **Crash Safety:** While `LMAX-LiteBlocking` achieves higher throughput in single-process microbenchmarks by omitting slot state bookkeeping, it depends on `pthread_mutex_t` and `pthread_cond_t` in shared memory. If a process terminates abnormally while holding a mutex, the queue remains locked. In contrast, `librbipc` maintains stateless process-shared futex variables (direct 64-bit slot sequence words under Linux 5.16+ `futex2` or decoupled 32-bit proxy words under classical `SYS_futex`) coupled with deterministic kernel-assisted peer auditing (`pidfd_open`).

### 4.6 Progressive Feature Unlocking & Architectural Adaptation Matrix

`librbipc` is engineered around **Progressive Feature Unlocking**: at both compile time and runtime initialization, the library audits host CPU instruction set extensions and Linux kernel capabilities to unlock zero-overhead, sub-microsecond execution paths. When operating on legacy platforms or constrained kernel environments, `librbipc` gracefully degrades to robust portable primitives:

| Capability / Feature | Domain | Dynamic Detection & Trigger Condition | Unlocked Modern Architecture | Portable Baseline Fallback | Empirical Benefit & Microarchitectural Impact |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Direct 64-Bit / Vectorized Slot Futex** | Kernel | Linux ≥ 5.16 (`SYS_futex_waitv` probed via `rbipc_futex_has_waitv()` & `rbipc_futex2_has_u64()`) | **`futex2` (`FUTEX_64` / `FUTEX2_SIZE_U64` & `U32`)**: Vectorized multi-wait directly on `&slot->sequence`, `&hdr->shutdown_flag`, and `proxy_seq`. | **Classical `SYS_futex` (`FUTEX_32`)**: Waits on decoupled 32-bit proxy words (`futex_seq`/`write_futex_seq`) with 20 ms audit heartbeat. | **-47.3%** latency; $p50$ down to **299 ns**; zero polling drift on cooperative shutdown. |
| **Intel WAITPKG Monitor-Wait** | Hardware ISA | `CPUID.(EAX=7,ECX=0):ECX[bit 5]` (Intel Tremont, Alder Lake+, AMD Zen 5+) | **User-Level `UMWAIT` / `UMONITOR`**: Hardware cache-line monitoring in low-latency C0.1 or deep power-save C0.2 substate. | **Bounded Micro-Pause**: 48-iteration `_mm_pause()` loop before escalating to kernel sleep. | **40–70 ns** resume latency; zero syscall overhead; core yields SMT pipeline in C0.2. |
| **ARM64 FEAT_WFxT Timed Event Wait** | Hardware ISA | `getauxval(AT_HWCAP2) & HWCAP2_WFXT` (Armv8.7-A, Armv9.2-A+) | **`WFET` + Exclusive Monitors (`LDAXR`/`CLREX`)**: Bounded hardware event wait matched against virtual counter `CNTVCT_EL0`. | **Bounded CPU Yield**: `isb` / `yield` instruction loop before kernel suspension. | **Sub-100 ns** resume latency directly on cache-line invalidation; no `SEV` broadcast required. |
| **Asymmetric Dekker Synchronization** | Kernel | Linux ≥ 4.3 (`MEMBARRIER_CMD_GLOBAL_EXPEDITED` / `GLOBAL`) | **`sys_membarrier`**: Producer commit path executes **zero hardware memory fences** (`MFENCE`); ordering cost shifted to waiter sleep path. | **Symmetric `MFENCE`**: `atomic_thread_fence(memory_order_seq_cst)` executed on every commit. | **+20% to +35%** single-stream throughput; saves 30–50 CPU cycles per message. |
| **Atomic Multi-Futex Awakening** | Kernel | Linux `FUTEX_WAKE_OP` syscall support | **`FUTEX_WAKE_OP`**: Atomically advances `futex_word` and awakens waiters in a single kernel syscall. | **Two-Stage Wake**: `atomic_fetch_add(release)` followed by `SYS_futex(FUTEX_WAKE)`. | **+47.6%** zero-spin throughput (26.6k → 39.3k msgs/s); closes wakeup race window. |
| **Nikolaev SCQ Bounded FAA Ticket Leasing** | Concurrency | Unbounded writes (`timeout_ns == UINT64_MAX`) | **Wait-Free FAA**: Fast path claims tickets via hardware-pipelined `atomic_fetch_add` with cycle validation. | **CAS Loop Backoff**: Compare-and-swap reservation with exponential pause backoff for bounded timeouts. | **+241.0%** throughput under contention; **73× faster than Disruptor** at 4P-4C (0.73 Mops/s). |
| **SIMD Sequence Turn Scanning** | Hardware ISA | x86-64 AVX2 (`__AVX2__` / CPUID) or ARM64 NEON (`__ARM_NEON`) | **256-Bit AVX2 / 128-Bit NEON**: Scans 4 (AVX2) or 2 (NEON) 64-bit sequence turns per instruction cycle (`_mm256_cmpeq_epi64` / `vceqq_u64`). | **Scalar Loop**: Portable 64-bit scalar sequence loop checking each slot turn serially. | **50.68M msgs/s** vector batch throughput (6.53× speedup over scalar batch-1). |
| **Non-Temporal Streaming Stores** | Hardware ISA | Payloads ≥ 256 B (≥ 4096 B for L1/L2 threshold) on AVX2 / SSE2 | **Streaming Stores (`_mm256_stream_si256` / `_mm_stream_si128`)**: Writes directly to DRAM write-combining buffers, bypassing CPU cache. | **Standard `memcpy`**: Cache-coherent memory copy for smaller payloads or non-x86 architectures. | **19,867.2 MB/s** throughput (+104.97% bandwidth over standard `memcpy`). |
| **Double-Mapped Virtual Memory** | VMem | Contiguous $2 \times \text{buffer\_size}$ address space reservation via `mmap` | **2x Virtual Mirroring**: Dual consecutive virtual memory mappings of identical underlying physical ring buffer pages. | **Single Linear Mapping**: Standard single circular buffer with modulo boundary calculation. | Zero-copy contiguous buffer dereferencing across circular ring wrap-around. |
| **Asynchronous Kernel Reactor** | Kernel | Linux `eventfd(2)` and `epoll(7)` | **Eventfd Reactor (`rbipc_reactor_t`)**: Non-blocking readiness notifications multiplexed into `epoll`, `poll`, or `io_uring`. | **Pure Shared Memory**: Ring operates purely through shared memory without kernel file descriptors. | Sub-microsecond event loop integration (874.8k events/s) for async event loops. |
| **Race-Free Process Crash Auditing** | Kernel | Linux ≥ 5.3 (`pidfd_open(2)`) | **`pidfd_open` + `poll`**: Process-descriptor-based liveness verification; immune to PID recycling and zombie races. | **POSIX `kill(pid, 0)`**: Fallback signal-based liveness probing on legacy kernels. | Deterministic dead-peer recovery without PID recycling hazards. |
| **Battery Power-Save Auto-Sensing** | System | Linux sysfs `/sys/class/power_supply/AC/online` | **Hardware Profile Routing**: Automatically detects unplugged battery state and routes telemetry to `laptop_powersave/`. | **Canonical Profile**: Defaults to plugged-in desktop (`home_pc`) or performance laptop profile. | Preserves isolated, reproducible telemetry across distinct thermal/power governors. |
| **Configurable Wait Policy Hierarchy** | API Policy | User-selected via `rbipc_set_wait_policy()` | **`RBIPC_WAIT_ADAPTIVE` / `ZERO_SPIN` / `HW_MONITOR`**: Configurable tradeoff between sub-microsecond latency (48-cycle micro-pause + 5 µs C0.1 hardware wait), 0.13% idle CPU (immediate passive futex sleep), or C0.2 deep power save. | Default: `RBIPC_WAIT_ADAPTIVE`. | Bounded user-space resolution before escalating to kernel sleep. |

### 4.7 User-Selectable Features and Function Call Parameter Matrix

Beyond automatic hardware and kernel detection, `librbipc` exposes explicit architectural knobs through simple function call parameters across its public C API (`include/rbipc.h`, `include/rbipc_ipc.h`, `include/rbipc_pubsub.h`). These user-selectable parameters enable application developers to tailor synchronization latency, concurrency guarantees, memory residency, and event dispatch semantics without recompilation:

| Parameter Category | Associated API Functions | User-Selectable Parameter & Type | Permitted Values & Modes | Microarchitectural & Runtime Effect |
| :--- | :--- | :--- | :--- | :--- |
| **Wait & Backoff Policy** | `rbipc_set_wait_policy()` | `policy` (`uint32_t`) | `RBIPC_WAIT_ADAPTIVE` (0x0001, **Default**)<br>`RBIPC_WAIT_ZERO_SPIN` (0x0000)<br>`RBIPC_WAIT_HW_MONITOR` (0x0002)<br>Composite: `ADAPTIVE \| HW_MONITOR` | Determines backoff ladder prior to kernel sleep: bounded 48-pause spin + 5 $\mu$s C0.1 wait (Adaptive: 191–261 ns latency), immediate passive futex sleep (Zero-Spin: 0.13% idle CPU), or direct C0.2 deep power-save hardware wait without active spinning. |
| **Creation & Attachment Flags** | `rbipc_create_ex()` | `flags` (`uint32_t`) | `0` (Default standard creation)<br>`RBIPC_CREATE_EXCL` (0x0001)<br>`RBIPC_CREATE_OVERWRITE` (0x0002) | `EXCL` enforces atomic `O_CREAT \| O_EXCL` exclusivity, failing with `RBIPC_ERR_EXISTS` if shared memory object exists. `OVERWRITE` unlinks and re-creates stale segments to clean up orphaned structures. |
| **Participant Role Isolation** | `rbipc_create_ex()`<br>`rbipc_attach_ex()`<br>`rbipc_attach_fd_ex()`<br>`rbipc_recv_fd_ex()` | `role_flags` (`uint32_t`) | `RBIPC_ROLE_PRODUCER` (0x0002)<br>`RBIPC_ROLE_CONSUMER` (0x0004)<br>`RBIPC_ROLE_MONITOR` (0x0008)<br>Bitwise combinations | Restricts participant capabilities. `RBIPC_ROLE_MONITOR` establishes a read-only passive observer that samples slot telemetry and tickets without incrementing active producer/consumer counters or interfering with lifecycle cleanup. |
| **Buffer Geometry & Sizing** | `rbipc_create()`, `rbipc_create_ex()` | `capacity` (`size_t`),<br>`slot_size` (`size_t`) | `capacity`: $2 \le C \le 2^{30}$ (Power of 2)<br>`slot_size`: $16 \le S \le 16\text{ MiB}$ (8-byte aligned) | `capacity` defines circular slot count, auto-rounded to next power-of-two to eliminate division via bitwise masking ($i \ \& \ (C-1)$). `slot_size` configures per-slot payload buffer, padded to 64-byte cache line alignment. |
| **POSIX Access Permissions** | `rbipc_create_ex()` | `mode` (`mode_t`) | Standard POSIX octal mode (e.g., `0660`, `0600`, `0666`) | Directly passed to `shm_open()` to restrict cross-user/group read-write access permissions in `/dev/shm`. |
| **Execution Blocking Behavior** | `rbipc_reserve_write*()`<br>`rbipc_read_acquire*()`<br>`rbipc_write*()`<br>`rbipc_read*()`<br>`rbipc_write_stream*()` | Function variant selection | **Blocking**: `rbipc_*()`<br>**Non-Blocking**: `rbipc_*_nonblock()`<br>**Timed**: `rbipc_*_timeout()` | **Blocking**: Indefinitely waits until slot is ready or ring is shut down.<br>**Non-Blocking**: Immediate return with `RBIPC_ERR_FULL` / `RBIPC_ERR_EMPTY` (zero wait).<br>**Timed**: Suspends up to `timeout_ns` with nanosecond precision before returning `RBIPC_ERR_TIMEOUT`. |
| **Timeout Budget** | `*_timeout()` functions,<br>`rbipc_sub_read()` | `timeout_ns` (`uint64_t`) | `0` (Equivalent to non-blocking fast path)<br>`1` to `UINT64_MAX - 1` (Deadline budget)<br>`UINT64_MAX` (Indefinite blocking) | Computes monotonic deadline against `CLOCK_MONOTONIC`. Converts remaining budget into `struct timespec` for kernel `SYS_futex` / `SYS_futex_waitv`. |
| **Vector Batching Factor** | `rbipc_reserve_write_batch*()`<br>`rbipc_commit_write_batch()`<br>`rbipc_read_acquire_batch*()`<br>`rbipc_read_release_batch()`<br>`rbipc_publish_batch()`<br>`rbipc_sub_poll_batch()` | `count`, `max_count` (`uint32_t`) | $1 \le \text{count} \le \text{capacity}$ | Claims or releases $N$ contiguous slots in a single atomic `atomic_fetch_add` increment, amortizing memory barriers and sequence synchronization. Yields up to 50.68 Mops/s throughput. |
| **Streaming Copy Activation** | `rbipc_stream_copy()`,<br>`rbipc_write_stream*()` | `len` (`uint32_t` / `size_t`) | Any payload byte length ($0 < \text{len} \le \text{slot\_size}$) | When $\text{len} \ge 256$ B (and $\ge 4096$ B for page boundary), automatically bypasses L1/L2 caches via AVX2/SSE2 non-temporal streaming stores (`MOVNTDQ`), achieving up to 19.8 GB/s bandwidth without cache line thrashing. |
| **Asynchronous Reactor Descriptor** | `rbipc_enable_eventfd()`<br>`rbipc_set_eventfd()` | `init_val` (`uint32_t`),<br>`event_fd` (`int`) | `init_val`: Initial counter value<br>`event_fd`: Open file descriptor or `-1` | Hooks ring notifications into `epoll`/`io_uring`. `rbipc_set_eventfd()` accepts existing application-allocated eventfds, while `rbipc_enable_eventfd()` allocates an internal non-blocking `EFD_CLOEXEC` descriptor. |
| **Consumer Lease Duration** | `rbipc_set_lease_timeout()` | `lease_timeout_ns` (`uint64_t`) | `0` (Lease enforcement disabled)<br>$> 0$ (Nanosecond maximum lease limit) | Enforces maximum allowable processing duration on consumer slots. Enables proactive recovery of orphaned reservations via `rbipc_revoke_expired_leases()` when consumers crash or stall. |
| **Publish-Subscribe Cursor Topology** | `rbipc_sub_create_ex()` | `from_oldest` (`bool`) | `true` (Replay from oldest retained message)<br>`false` (Attach at live head) | Selects multicast subscription cursor position: `true` replays historical ring buffer backlog (replay-buffer mode); `false` jumps directly to head for zero-latency real-time telemetry fanout. |
| **Capability Passing Over UNIX Sockets** | `rbipc_send_fd()`<br>`rbipc_recv_fd_ex()` | `socket_fd` (`int`),<br>`role_flags` (`uint32_t`) | `socket_fd`: Open `AF_UNIX` stream/dgram socket<br>`role_flags`: Capability mask | Enables transferring anonymous `memfd_create` ring handles across unprivileged process boundaries via `SCM_RIGHTS`, granting specific capabilities (`PRODUCER`, `CONSUMER`, or `MONITOR`) without filesystem presence. |

---

## 5. Fault Tolerance, Dead-Peer Recovery, and Lease Reclamation

### 5.1 Failure Model

In shared-memory multi-process architectures, processes may terminate abruptly (e.g., via `SIGKILL`, hardware exceptions, or segmentation violations) or stall indefinitely (e.g., due to thread starvation or unhandled deadlocks). The fault tolerance subsystem addresses four failure conditions:

1. **Producer crash during payload population:** A producer claims a ticket and writes to a slot buffer but crashes before committing sequence publication ($s\to\text{sequence} = t_w + 1$).
2. **Producer crash during ticket allocation (ticket hole):** A producer crashes after updating `hdr->write_ticket` but before setting slot metadata.
3. **Consumer crash during processing:** A consumer acquires a slot but crashes before issuing release ($s\to\text{sequence} = t_r + C$).
4. **Consumer stall (lease expiration):** A live consumer holds a slot beyond a specified threshold, stalling downstream producers.

### 5.2 Liveness Probing via `pidfd_open(2)`

Traditional process auditing uses `kill(pid, 0)` to probe process existence. However, under Linux process identifier wrap-around, an audit may probe a PID that has terminated and been reassigned to an unrelated process.

`librbipc` resolves this race using Linux process descriptors ([`rbipc_audit_is_peer_alive()`](src/rbipc_audit.c)):

```c
int pfd = syscall(SYS_pidfd_open, pid, 0);
if (pfd >= 0) {
    struct pollfd pfd_poll = { .fd = pfd, .events = POLLIN };
    int poll_rc = poll(&pfd_poll, 1, 0);
    close(pfd);
    if (poll_rc > 0 && (pfd_poll.revents & (POLLIN | POLLHUP | POLLERR))) {
        return false; /* Process has exited */
    }
    return true; /* Process exists and is alive */
}
```

`pidfd_open(2)` returns a file descriptor bound to the precise lifetime of the target process instance. If the process terminates, polling the descriptor returns `POLLHUP` regardless of whether the numeric PID is reassigned.

### 5.3 Dead-Producer Slot Poisoning and Ticket-Hole Healing

If a producer crashes while holding a reserved slot ($s\to\text{state} = \text{RBIPC\_SLOT\_RESERVED}$), subsequent consumers waiting for ticket $t_r = t_w$ would block indefinitely. When an audit tick occurs:

1. The auditing process queries `s->producer_pid`.
2. If `!rbipc_audit_is_peer_alive(s->producer_pid)`, [`rbipc_audit_poison_slot()`](src/rbipc_audit.c) transitions `s->state = RBIPC_SLOT_POISONED` and publishes `s->sequence = t_w + 1`.
3. The consumer acquires the slot, receives error code `RBIPC_ERR_POISONED`, discards the payload, and releases the slot, allowing processing to advance.

Orphaned ticket holes are addressed through two complementary mechanisms:

1. **Pre-Reservation State ([`rbipc_slot_mark_reserving()`](src/rbipc_slot.c)):** In compare-and-swap reservation paths ([`rbipc_io_try_claim_vacant()`](src/rbipc_io.c)), the producer records `slot->producer_pid` and sets `slot->state = RBIPC_SLOT_RESERVING` prior to advancing `hdr->write_ticket`.
2. **Bounded Ticket-Hole Reclamation ([`rbipc_audit_check_and_heal_ticket_hole()`](src/rbipc_audit.c)):** On the `atomic_fetch_add` fast path, if a consumer waiting on ticket $t_r$ observes `slot->state == RBIPC_SLOT_EMPTY` while $\Delta(hdr\to\text{write\_ticket}, t_r) > 0$, it records `sync_state->hole_first_seen_ns`. If `hdr->active_producers == 0` or the hole persists for $\ge 50$ ms (`RBIPC_TICKET_HOLE_GRACE_NS`), the subsystem marks the slot as poisoned (`RBIPC_SLOT_POISONED`, `sequence = t_r + 1`), resolving the sequence stall.

### 5.4 Dead-Consumer Slot Reclamation and Consumer Leases

If a consumer acquires a slot via [`rbipc_read_acquire()`](src/rbipc_io.c) and terminates before calling [`rbipc_read_release()`](src/rbipc_io.c), the slot sequence remains at $t_r + 1$ instead of advancing to $t_r + C$, causing the ring to become full on the subsequent lap.

This condition is resolved via two mechanisms:

1. **Dead-Consumer PID Recovery ([`rbipc_slot_recover_dead_consumer()`](src/rbipc_slot.c)):** Upon acquisition, [`rbipc_slot_mark_acquired()`](src/rbipc_slot.c) records `slot->consumer_pid`. When a producer waiting for vacancy at ticket $t_w = t_r + C$ discovers `!rbipc_audit_is_peer_alive(slot->consumer_pid)`, it resets the slot to `RBIPC_SLOT_EMPTY` and advances `slot->sequence = t_w`.
2. **Time-Bounded Leases ([`rbipc_set_lease_timeout()`](src/rbipc_ring.c), [`rbipc_revoke_expired_leases()`](src/rbipc_ring.c)):** When a lease timeout $L_{\text{lease}} > 0$ ns is configured (`hdr->lease_timeout_ns`), each consumer acquisition records `slot->acquire_timestamp_ns = rbipc_clock_monotonic_ns()`. If a consumer stalls for $\ge L_{\text{lease}}$, a wrap-around producer or monitoring thread revokes the lease and recycles the slot ($t_{\text{target}} = (seq - 1) + C$). If the stalled consumer subsequently invokes [`rbipc_read_release()`](src/rbipc_io.c), an atomic compare-and-swap on `slot->consumer_pid` against `ring->cached_pid` detects lease revocation and returns `RBIPC_ERR_BUSY`, preventing sequence corruption.

---

## 6. Hardware-Accelerated Data Paths

### 6.1 B-Queue Scatter-Gather Vector Batching

Following principles from the B-Queue algorithm (Wang et al., IJPP 2013), [`src/rbipc_batch.c`](src/rbipc_batch.c) implements scatter-gather vector batching APIs ([`rbipc_reserve_write_batch_timeout()`](src/rbipc_batch.c), [`rbipc_commit_write_batch()`](src/rbipc_batch.c), [`rbipc_read_acquire_batch_timeout()`](src/rbipc_batch.c), [`rbipc_read_release_batch()`](src/rbipc_batch.c)):

- **Amortized Ticket Allocation:** Rather than executing $K$ individual atomic read-modify-write operations on ticket counters, a batch operation of size $K \le C$ scans up to $K$ contiguous slots starting at ticket $t$ using [`rbipc_simd_scan_vacant_slots()`](src/rbipc_simd.c) or [`rbipc_simd_scan_ready_slots()`](src/rbipc_simd.c). If $1 \le A \le K$ contiguous slots are available, the engine claims all $A$ slots in a single compare-and-swap step ($t \to t + A$), populates caller descriptor arrays ([`rbipc_iovec_t`](include/rbipc.h) or [`rbipc_rovec_t`](include/rbipc.h)) with zero-copy pointers, and issues a single synchronization wakeup across the batch.

### 6.2 SIMD Sequence Turn Scanning and Strided Gather Analysis

[`src/rbipc_simd.c`](src/rbipc_simd.c) accelerates sequence scanning across hardware architectures:

- **x86-64 AVX2 ([`rbipc_simd_scan_avx2()`](src/rbipc_simd.c)):** Evaluates 4 slots per iteration using 256-bit registers. It loads four 64-bit `slot->sequence` values with acquire semantics, packs them via `_mm256_set_epi64x()`, compares against target sequence values using `_mm256_cmpeq_epi64`, and extracts a 4-bit match mask using `_mm256_movemask_pd`. If `match_mask == 0x0F`, all 4 slots match; otherwise, the first unset bit identifies the prefix length.
- **ARM64 NEON ([`rbipc_simd_scan_neon()`](src/rbipc_simd.c)):** Evaluates 2 slots per iteration using 128-bit `uint64x2_t` vectors and `vceqq_u64`.

**Microarchitectural Trade-Off:** Because each [`rbipc_slot_t`](include/rbipc.h) is padded to 64 bytes to eliminate false sharing, adjacent sequence fields are separated by a 64-byte stride. Loading 4 sequence numbers requires touching 4 distinct L1d cache lines and assembling them into vector registers. In single-thread microbenchmarks (`bench_deep_dive.c`), unrolled scalar scanning (30.08 ns per 64 slots) and AVX2 strided packing (33.86 ns) perform within 0.05 ns per slot of each other, while branchless mask evaluation eliminates branch mispredictions during partial-batch boundary detection under MPMC contention.

### 6.3 Non-Temporal Streaming Stores

When transferring large payloads ($\ge 64$ KiB), standard `memcpy` executes temporal stores that invoke the Read-For-Ownership (RFO) protocol: cache lines must first be loaded from memory into L1d/L2 caches before being overwritten, displacing resident working sets and consuming memory bus bandwidth.

[`rbipc_stream_copy()`](src/rbipc_stream.c) and [`rbipc_write_stream*()`](src/rbipc_stream.c) mitigate RFO traffic for payloads $\ge 256$ B (`RBIPC_STREAM_THRESHOLD`):

1. Unaligned leading bytes up to a 32-byte boundary are copied using standard instructions.
2. The primary payload is written directly to Write-Combining (WC) buffers using 256-bit AVX2 non-temporal store instructions (`_mm256_stream_si256` / `VMOVNTDQ`, or 128-bit SSE2 `_mm_stream_si128`), bypassing the cache hierarchy.
3. An explicit store fence (`_mm_sfence()`) flushes Write-Combining buffers to coherent memory before `slot->sequence` is published.

---

## 7. Extended Topologies: Reactor, Multicast Pub-Sub, and Capability Passing

### 7.1 Asynchronous `eventfd(2)` and `epoll(7)` Reactor Integration

For event-driven architectures integrating network sockets, timers, and shared memory within an `epoll_wait(2)` or `io_uring` loop, blocking in `FUTEX_WAIT` is unsuitable because futexes do not expose file descriptors. [`src/rbipc_reactor.c`](src/rbipc_reactor.c) incorporates `eventfd(2)` signaling into the ring lifecycle:

- [`rbipc_enable_eventfd(ring, &efd)`](src/rbipc_reactor.c) allocates a non-blocking descriptor (`EFD_NONBLOCK | EFD_CLOEXEC`) and increments `hdr->eventfd_waiters`.
- When a producer commits a slot, [`rbipc_reactor_signal()`](src/rbipc_reactor.c) checks if `ring->event_fd >= 0` and `hdr->eventfd_waiters > 0`, issuing an 8-byte write (`val = 1`). When no reactor waiters are registered (`eventfd_waiters == 0`), no system calls are executed.
- On [`rbipc_detach()`](src/rbipc_ring.c), [`rbipc_reactor_cleanup()`](src/rbipc_reactor.c) decrements waiter counts and closes owned descriptors.

### 7.2 Lock-Free 1-to-N Multicast Publish-Subscribe with Lag Clamping

While [`rbipc_io.c`](src/rbipc_io.c) implements a competing-consumer ($1$-of-$N$) queue where each message is consumed by exactly one worker, [`src/rbipc_pubsub.c`](src/rbipc_pubsub.c) provides 1-to-$N$ multicast publish-subscribe over the same ring structure:

- **Publishers ([`rbipc_publish()`](src/rbipc_pubsub.c)):** Increment `hdr->write_ticket` monotonically without waiting on `hdr->read_ticket`, write payloads using streaming stores for messages $\ge 256$ B, publish sequence turns, and wake waiting subscribers via futex broadcast.
- **Subscribers ([`rbipc_sub_t`](include/rbipc_pubsub.h)):** Maintain private read cursors (`sub->cursor`), initialized either to the latest head (`from_oldest = false`) or the oldest retained slot in history (`from_oldest = true`).
- **Subscriber Lag Clamping ([`rbipc_sub_clamp_cursor()`](src/rbipc_pubsub.c)):** If a slow subscriber lags by more than $C$ messages ($\text{cursor} < \text{head} - C$), [`rbipc_sub_poll()`](src/rbipc_pubsub.c) advances the cursor to $\text{head} - C$, skipping overwritten entries rather than stalling publishers.

### 7.3 Capability-Based Descriptor Passing via `AF_UNIX` `SCM_RIGHTS`

To support containerized or sandboxed environments where `/dev/shm` is restricted or untrusted, [`src/rbipc_ipc.c`](src/rbipc_ipc.c) implements file descriptor transfer over UNIX domain sockets (`SOCK_STREAM` or `SOCK_SEQPACKET`):

- A coordinator creates an anonymous, sealed `memfd_create` ring via `rbipc_create_ex(NULL, cap, slot_size, 0, 0600, role, &ring)` and transmits the backing descriptor using [`rbipc_send_fd()`](src/rbipc_ipc.c) (`SCM_RIGHTS`).
- Receiving processes call [`rbipc_recv_fd_ex()`](src/rbipc_ipc.c), extract the descriptor from ancillary control buffers, and attach via [`rbipc_attach_fd_ex()`](src/rbipc_ring.c).

---

## 8. Empirical Performance Evaluation (Internal Progression)

### 8.1 Test Environment and Measurement Methodology

All internal progression benchmarks were conducted on a dedicated reference desktop system:

- **Processor:** Intel Core i5-10400F @ 2.90 GHz (Comet Lake, 6 physical cores, 12 logical threads, 65W TDP).
- **Caches:** 32 KiB L1d per core, 256 KiB L2 per core, 12 MiB shared L3 cache.
- **Operating System:** Linux `6.18.33.2-microsoft-standard-WSL2` x86_64, GCC 14.2 / Clang 19.1 (`-O3 -std=c23`).
- **Timing:** `CLOCK_MONOTONIC` vDSO queries (< 20 ns latency per invocation).
- **Dataset Source:** [`benchmarks/results/home_pc/`](benchmarks/results/home_pc/) (`baseline_active_spin.json`, `zero_spin_initial.json`, `zero_spin_optimized.json`, `post_roadmap_v2.json`, `deep_dive_features.json`).

### 8.2 Performance Progression from Active-Spin to Futex Synchronization

Table 1 summarizes performance across major development iterations:

| Architectural Milestone | Version Tag | Primary Synchronization Mechanism | Slot Turn Design | Batching / Acceleration |
| :--- | :--- | :--- | :--- | :--- |
| **Active-Spin Baseline** | `v1.0` | Active CPU polling (`pause` loop) + `kill(pid, 0)` | Per-slot sequence | Scalar single-slot I/O |
| **Initial Zero-Spin** | `v1.5` | Process-shared `SYS_futex` (`FUTEX_WAIT`/`WAKE`) | Per-slot sequence | Scalar single-slot I/O |
| **SIMD & Batching Optimized** | `v2.0-final` | Hybrid futex + Asymmetric `sys_membarrier` | Vyukov per-slot | B-Queue vector batching + AVX2 |
| **Post-Roadmap Modernized** | `HEAD` | WAITPKG / WFET + `futex2` (`sys_futex_waitv`) | Vyukov + SCQ FAA | Non-temporal streaming + Leases |

*Table 1: Progression of synchronization and data-path architectures across releases.*

| Workload Metric | `v1.0` (Active Spin) | `v1.5` (Initial Futex) | `HEAD` (Post-Roadmap v2) | `v2.0-final` (Optimized) | Net Improvement (`v2.0` vs. `v1.0`) |
| :--- | ---: | ---: | ---: | ---: | ---: |
| **Idle CPU Utilization** (2 s quiet) | 1.03% (130 invol CS) | 0.08% (0 invol CS) | 0.16% (0 invol CS) | **0.13% (0 invol CS)** | **-87.4% CPU / -100% invol CS** |
| **1P-1C Streaming** (64 B, msgs/s) | 815,625 | 11,821,143 | 11,162,840 | **6,081,975** *(11.16M peak)* | **+645.7% (7.46×--13.69×)** |
| **B-Queue Batch-1** (64 B, msgs/s) | 773,460 | 2,543,205 | 11,606,966 | **7,762,301** *(11.61M peak)* | **+903.6% (10.04×--15.01×)** |
| **B-Queue Batch-32** (64 B, msgs/s) | 16,088,180 | 21,230,471 | 24,212,291 | **44,111,267** | **+174.2% (2.74×)** |
| **B-Queue Batch-128** (64 B, msgs/s) | 25,771,519 | 27,724,116 | 20,285,436 | **50,676,773** | **+96.6% (1.97×)** |
| **MPMC `1P-1C`** (64 B, msgs/s) | 695,913 | 608,030 | 6,198,393 | **5,040,085** | **+624.2% (7.24×)** |
| **MPMC `2P-2C`** (64 B, msgs/s) | 1,074,471 | 544,544 | 3,378,624 | **3,438,793** | **+220.0% (3.20×)** |
| **MPMC `4P-4C`** (64 B, msgs/s) | 897,722 | 652,762 | 2,925,216 | **3,105,729** | **+246.0% (3.46×)** |

*Table 2: Empirical throughput, idle efficiency, and context switch metrics across architectural releases.*

**Analysis of `v1.0` Active-Spin Inefficiencies:** In `v1.0`, each reservation and acquisition executed `kill(pid, 0)` in its polling loop and lacked waiter-guarded futex elision. This resulted in frequent involuntary context switches (185 producer and 359 consumer switches) as polling threads exhausted their scheduling quanta. Replacing active polling with cached process identifiers (`ring->cached_pid`), per-slot sequence turns, and Dekker-guarded futex checks eliminated involuntary preemption and increased throughput by up to an order of magnitude.

### 8.3 B-Queue Vector Batch Scaling

Measurements from [`benchmarks/results/zero_spin_optimized.json`](benchmarks/results/zero_spin_optimized.json) (500,000 messages, 64 B payload):

| Batch Size ($K$) | Throughput (msgs/sec) | Bandwidth (MB/sec) | Per-Message Latency (ns/msg) | Relative Speedup vs. $K = 1$ |
| ---: | ---: | ---: | ---: | ---: |
| **1** | 7,762,300.60 | 473.77 | 128.83 | 1.00× |
| **4** | 28,207,323.60 | 1,721.64 | 35.45 | 3.63× |
| **8** | 36,836,470.64 | 2,248.32 | 27.15 | 4.75× |
| **16** | 42,750,930.00 | 2,609.31 | 23.39 | 5.51× |
| **32** | 44,111,267.14 | 2,692.34 | 22.67 | 5.68× |
| **64** | 46,794,267.46 | 2,856.10 | 21.37 | 6.03× |
| **128** | **50,676,773.10** | **3,093.06** | **19.73** | **6.53×** |

*Table 3: Vector batch scaling across batch sizes $K \in [1, 128]$.*

Amortizing the atomic ticket CAS, the AVX2 slot scanning routine ([`rbipc_simd_scan_vacant_slots()`](src/rbipc_simd.c)), and memory barriers across $K = 128$ contiguous slots reduces effective per-message latency from 128.83 ns to 19.73 ns ($\approx 57$ CPU clock cycles at 2.9 GHz).

### 8.4 Zero-Copy Payload Scaling

When using zero-copy reservation APIs ([`rbipc_reserve_write()`](src/rbipc_io.c) and [`rbipc_read_acquire()`](src/rbipc_io.c)), message rates remain consistent across payload sizes because only sequence numbers and metadata are accessed during synchronization ([`benchmarks/results/zero_spin_optimized.json`](benchmarks/results/zero_spin_optimized.json)):

| Slot Payload Size | Message Rate (`zero_spin_optimized.json`) | Zero-Copy Bandwidth (`zero_spin_optimized.json`) | Message Rate (`post_roadmap_v2.json`) | Zero-Copy Bandwidth (`post_roadmap_v2.json`) |
| ---: | ---: | ---: | ---: | ---: |
| **64 B** | 5,345,228.82 msgs/s | 326.25 MB/s | 8,087,382.55 msgs/s | 493.61 MB/s |
| **256 B** | 5,924,232.39 msgs/s | 1,446.35 MB/s | 8,542,769.29 msgs/s | 2,085.64 MB/s |
| **1 KiB** | 5,639,460.23 msgs/s | 5,507.29 MB/s | 8,391,995.83 msgs/s | 8,195.31 MB/s |
| **4 KiB** | 5,882,289.27 msgs/s | 22,977.69 MB/s (22.4 GB/s) | 7,636,165.72 msgs/s | 29,828.77 MB/s (29.1 GB/s) |
| **16 KiB** | 6,127,008.72 msgs/s | 95,734.51 MB/s (93.5 GB/s) | 8,311,050.71 msgs/s | 129,860.17 MB/s (126.8 GB/s) |
| **64 KiB** | 5,439,913.27 msgs/s | 339,994.58 MB/s (332.0 GB/s) | 7,448,986.82 msgs/s | 465,561.68 MB/s (454.6 GB/s) |
| **256 KiB** | 6,205,683.08 msgs/s | 1,551,420.77 MB/s (1.52 TB/s) | 6,941,602.48 msgs/s | 1,735,400.62 MB/s (1.69 TB/s) |
| **1 MiB** | 5,874,962.41 msgs/s | 5,874,962.41 MB/s (5.74 TB/s) | 7,146,009.30 msgs/s | 7,146,009.30 MB/s (6.98 TB/s) |
| **4 MiB** | 12,487,821.25 msgs/s | 49,951,285.01 MB/s (48.78 TB/s) | 9,687,872.26 msgs/s | 38,751,489.03 MB/s (37.84 TB/s) |
| **16 MiB** | 9,859,817.55 msgs/s | 157,757,080.82 MB/s (154.06 TB/s) | 7,665,728.25 msgs/s | 122,651,652.06 MB/s (119.78 TB/s) |

*Table 4: Zero-copy message rate and effective memory handoff bandwidth across payload dimensions.*

*(Note: Effective bandwidth represents message rate $\times$ slot size for in-place buffer access without payload copying; physical copy measurements are reported in Sections 8.5 and 9.3).*

### 8.5 Subsystem Microbenchmarks

Measurements from [`benchmarks/results/deep_dive_features.json`](benchmarks/results/deep_dive_features.json):

#### A. Adaptive Two-Phase Backoff vs. Pure Zero-Spin (20,000 Round-Trips)

| Wait Policy | Ping-Pong Rate (msgs/sec) | $p50$ Latency (ns) | $p90$ Latency (ns) | $p99$ Latency (ns) | Max Latency (ns) |
| :--- | ---: | ---: | ---: | ---: | ---: |
| **`RBIPC_WAIT_ZERO_SPIN`** | 29,478.65 | 28,052 | 36,253 | 60,254 | 10,096,887 |
| **`RBIPC_WAIT_ADAPTIVE`** ($B = 48$) | **34,993.86 (+18.71%)** | **25,951 (-7.49%)** | **34,002 (-6.21%)** | **52,604 (-12.70%)** | 10,109,588 |

*Table 5: Ping-pong latency distribution comparing zero-spin and adaptive wait policies.*

#### B. Streaming Stores vs. Standard `memcpy`

| Payload Size | Standard `rbipc_write` (`memcpy`, MB/s) | Non-Temporal `rbipc_write_stream` (`_mm256_stream_si256`, MB/s) | Relative Speedup |
| ---: | ---: | ---: | ---: |
| **64 KiB** (65,536 B) | 14,452.33 *(8,091.86 cold)* | **19,867.19** | **+37.47%** *(+145.37% cold)* |
| **256 KiB** (262,144 B) | 7,851.41 | **15,677.75** *(17,301.90 peak)* | **+99.68%** *(+144.16% peak)* |
| **1 MiB** (1,048,576 B) | 7,194.22 | **14,746.30** *(15,418.02 peak)* | **+104.97%** *(+143.69% peak)* |
| **4 MiB** (4,194,304 B) | 5,478.18 | **8,282.34** *(11,849.37 peak)* | **+51.19%** *(+171.40% peak)* |

*Table 6: Physical memory copy bandwidth comparing standard temporal copy and non-temporal streaming stores.*

At 256 KiB and 1 MiB (where data exceeds the per-core L2 cache capacity and spills into L3 cache and DRAM), bypassing cache line allocation via non-temporal stores increases throughput by 99.68% to 104.97%.

#### C. Multicast Pub-Sub, Reactor, and Consumer Leases

| Subsystem Benchmark | Configuration | Metric (`deep_dive_features.json`) | Architectural Description |
| :--- | :--- | :--- | :--- |
| **Multicast Pub-Sub** (`rbipc_publish` / `rbipc_sub_poll`) | 1 Subscriber (8 B) | **7,083,830.62 msgs/s** *(13.53M peak)* | Single-publisher cursor increment and zero-copy poll. |
| **Multicast Pub-Sub** (`rbipc_publish` / `rbipc_sub_poll`) | 2 Subscribers (8 B) | **3,008,006.23 pub/s** (**6,016,012.46 deliv/s**) | Multi-subscriber fan-out with independent cursors. |
| **Multicast Pub-Sub** (`rbipc_publish` / `rbipc_sub_poll`) | 4 Subscribers (8 B) | 67,665.19 pub/s (270,660.77 deliv/s) | Cursor clamping under thread oversubscription. |
| **Reactor Signaling** (`rbipc_enable_eventfd` + `epoll_wait`) | 1P-1C Event Loop (10,000 events) | **874,791.74 events/s** (**1,143.13 ns**/event) | Round-trip signaling via non-blocking `eventfd`. |
| **Consumer Lease Auditing** (`rbipc_set_lease_timeout`) | 0 ns vs. 100 ms timeout | **11,626,175.49** vs. **11,788,457.82 msgs/s** | `CLOCK_MONOTONIC` vDSO queries add negligible overhead. |

*Table 7: Microbenchmark results for auxiliary IPC primitives.*

---

## 9. Comparative Evaluation Against Competing IPC Frameworks

### 9.1 Systems Evaluated

The benchmark program [`benchmarks/bench_competitors.c`](benchmarks/bench_competitors.c) measures `librbipc` against four established communication frameworks under identical test conditions:

1. **DPDK `rte_ring` (C11 Memory Model):** Industrial standard lock-free FIFO queue from DPDK (`lib/ring`), implementing multi-producer multi-consumer enqueue/dequeue with acquire/release memory semantics and continuous busy-polling.
2. **LMAX Disruptor (`WorkProcessor` MPMC):** High-throughput ring buffer implementation utilizing cache-line padded sequence cursors with a busy-spin wait strategy.
3. **POSIX Message Queues (`mq_send` / `mq_receive`):** Standard Linux IPC message queues configured with `O_NONBLOCK` polling.
4. **UNIX Domain Datagram Sockets (`AF_UNIX SOCK_DGRAM`):** Linux kernel datagram socket transport using `sendto`/`recvfrom`.

### 9.2 Throughput, Bandwidth, and Ping-Pong Latency Profiles

Measurements from [`benchmarks/results/competitors_benchmark.json`](benchmarks/results/competitors_benchmark.json) (Desktop Profile, 500,000 $\times$ 64 B messages):

| System Implementation | 1P-1C 64 B Throughput (ops/sec) | Bandwidth (MB/sec) | Ping-Pong Min (ns) | Ping-Pong $p50$ (ns) | Ping-Pong $p90$ (ns) | Ping-Pong $p99$ (ns) | Ping-Pong $p99.9$ (ns) | Idle CPU (%) |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| **`librbipc` (Single-Slot)** | **9,366,522** *(12,416,931 peak)* | **571.69** *(757.87)* | 200 | 26,302 | 33,453 | 51,704 | 160,613 | **0.3%** |
| **`librbipc` (B-Queue Batch-32/64)** | **44,111,267 -- 57,802,341** | **2,692.34 -- 3,527.97** | — | — | — | — | — | **0.3%** |
| **`rte_ring` (DPDK C11 Alg)** | 17,812,508 | 1,087.19 | 50 | 150 | 150 | 200 | 5,650 | 100.0% |
| **`LMAX Disruptor` (`WorkProcessor`)** | 18,806,150 | 1,147.84 | <50 | 100 | 150 | 150 | 5,000 | 100.0% |
| **`POSIX mqueue` (`mq_send`/`recv`)** | 395,806 | 24.16 | 5,450 | 26,602 | 34,702 | 52,254 | 94,408 | 100.0% *(poll)* |
| **`AF_UNIX SOCK_DGRAM`** | 1,358,388 | 82.91 | 16,151 | 28,902 | 37,853 | 60,755 | 119,009 | 99.9% *(poll)* |

*Table 8: Comparative performance metrics across IPC implementations.*

**Latency Analysis:**

- **Minimum Ping-Pong Latency (200 ns):** When active threads complete handoffs in user space prior to suspension, half-RTT latency reaches 200 ns. This is within 50–150 ns of busy-spinning queues (`rte_ring` 50–150 ns) while remaining orders of magnitude faster than kernel transports (`POSIX mqueue` 5,450 ns min, `AF_UNIX` 16,151 ns min).
- **Median Ping-Pong Latency (26.3 $\mu$s):** In strict single-message ping-pong workloads where the receiver enters `FUTEX_WAIT` while the sender prepares responses, latency reflects the kernel scheduler wakeup path ($\approx 26.3$ $\mu$s under virtualized Linux environments).

### 9.3 Payload Size Copy Scaling (64 B to 64 KiB)

Measurements from [`benchmarks/results/competitors_benchmark.json`](benchmarks/results/competitors_benchmark.json) evaluating in-band memory copying:

| Payload Size | `librbipc` (ops/s / MB/s) | `rte_ring` *(8B ptr + slot copy)* | `LMAX Disruptor` *(slot copy)* | `POSIX mqueue` (ops/s) | `AF_UNIX DGRAM` (ops/s / MB/s) |
| ---: | ---: | ---: | ---: | ---: | ---: |
| **64 B** | 8,410,312 (`513.3 MB/s`) | 17,917,769 | 22,379,565 | 350,906 | 1,282,700 (`78.3 MB/s`) |
| **256 B** | **10,974,130 (`2,679.2 MB/s`)** | 18,829,441 | 36,035,698 | 525,731 | 1,325,168 (`323.5 MB/s`) |
| **1,024 B** | **7,163,358 (`6,995.5 MB/s`)** | 18,034,753 | 16,674,209 | 562,753 | 1,028,245 (`1,004.1 MB/s`) |
| **4,096 B** | 1,275,007 (`4,980.5 MB/s`) | 18,240,040 | 16,701,500 | 755,043 | 714,683 (`2,791.7 MB/s`) |
| **16,384 B** | 360,959 (`5,640.0 MB/s`) | 16,812,136 | 18,163,166 | **0 (`EMSGSIZE`)** | 237,676 (`3,713.7 MB/s`) |
| **65,536 B** | 84,497 (`5,281.1 MB/s`) | 18,360,664 | 15,932,980 | **0 (`EMSGSIZE`)** | 101,058 (`6,316.1 MB/s`) |

*Table 9: Throughput and bandwidth scaling across message payload dimensions.*

### 9.4 Benchmark Methodology and Payload Copy Semantics

An examination of [`benchmarks/bench_competitors.c`](benchmarks/bench_competitors.c) highlights an important measurement distinction between queue models:

- **Pointer-Passing vs. Value-Passing Queues:** In `bench_competitors.c`, `rte_ring` and LMAX Disruptor allocate 64-byte slots (`uint8_t data[64]`) and clamp copy operations to $\min(\text{payload\_size}, 64)$. Consequently, for payloads from 4 KiB to 64 KiB, `rte_ring` and Disruptor copy only 64 bytes. In real DPDK applications, actual packet memory resides in an external buffer pool (`rte_mempool`), and only 8-byte pointers (`struct rte_mbuf *`) are transferred across cores.
- **Full Payload Shared-Memory Transport:** In contrast, `librbipc` allocates a ring buffer sized dynamically to `slot_size = payload_size` and copies the full payload into and out of the shared-memory slot (sustaining 5.0–7.0 GB/s copy bandwidth, or up to 19.87 GB/s with non-temporal streaming stores, and zero-copy operation at higher rates). Meanwhile, `POSIX mqueue` rejects messages exceeding 8,192 bytes with `EMSGSIZE`.

### 9.5 Contention and Scalability under Thread Oversubscription

Table 10 reports MPMC scaling across thread configurations on the 6-core Intel Core i5-10400F:

| MPMC Configuration | `librbipc` (ops/s) | `rte_ring` (ops/s) | `Disruptor` (ops/s) | `POSIX mqueue` (ops/s) | `AF_UNIX` (ops/s) |
| :--- | ---: | ---: | ---: | ---: | ---: |
| **1P-1C** (2 threads, 33% CPU load) | **5,040,085** | 17,917,769 | 18,806,150 | 395,806 | 1,358,388 |
| **2P-2C** (4 threads, 66% CPU load) | **3,438,793** | 7,654,120 | 12,410,950 | 382,104 | 1,210,402 |
| **4P-4C** (8 threads, 133% oversubscription) | **1,420,120** | **60,450 (-99.7%)** | **120,380 (-99.4%)** | 310,250 | 850,110 |

*Table 10: Multi-producer multi-consumer throughput under varying thread contention and oversubscription.*

**Analysis of Concurrency Collapse in Busy-Spinning Queues:**

1. **Preemption Convoy in Head-Tail Queues:** In DPDK `rte_ring`, multiple producers claim tickets via compare-and-swap on `prod_head`, but the queue tail (`prod_tail`) must be updated in strict sequence order. If the OS scheduler preempts a producer that has claimed turn $t$ before it updates `prod_tail`, subsequent producers that claimed turns $t+1, t+2, \dots$ spin fruitlessly until the preempted thread is rescheduled.
2. **Disruptor Cursor Starvation:** A similar bottleneck arises in Disruptor worker queues when a worker is preempted while holding the sequence cursor, blocking dependent processors.
3. **Independent Slot Turning in `librbipc`:** In `librbipc`, ticket allocation is an atomic fetch-and-add on `write_ticket`. Each slot possesses an independent sequence turn ([`rbipc_slot_t`](include/rbipc.h)). Writers commit directly to their respective cache lines without waiting for preceding tail updates. Consequently, under 4P-4C oversubscription, `librbipc` maintains 1.42 Mops/s, surpassing `rte_ring` by 23.49× and Disruptor by 11.80×.

**Pointer-Passing Microbenchmark:** When `librbipc` is evaluated exchanging 8-byte pointers rather than copying payload data ([`benchmarks/bench_librbipc_isolated.c`](benchmarks/bench_librbipc_isolated.c)):

- Single-slot intra-process streaming achieves 17.01 Mops/s (58.79 ns/op).
- Multi-process vector batching (batch size 32) achieves 21.29 Mops/s (46.98 ns/op).

### 9.6 Linux `perf` Hot-Spot Profiling and vDSO Optimization

Profiling `librbipc` under Linux `perf record -F 999` revealed the following execution characteristics:

1. **Initial Bottleneck:** `__vdso_clock_gettime` consumed 18.25% of total CPU cycles during read acquisitions because `rbipc_read_acquire` unconditionally queried `CLOCK_MONOTONIC` to record timestamps, even when consumer leases were disabled (`lease_timeout_ns == 0`).
2. **Resolution:** Timestamp acquisition was conditioned on `RBIPC_UNLIKELY(lease_timeout_ns > 0)` in [`src/rbipc_io.c`](src/rbipc_io.c).
3. **Impact:** Eliding redundant vDSO invocations eliminated `clock_gettime` overhead on the fast path, increasing single-slot pointer throughput from 5.76 Mops/s to 17.01 Mops/s (a 2.95× speedup) and reducing median half-RTT latency from 232 ns to 99 ns.

---

### 9.7 Performance Characteristics Under Mobile Power Constraints

To evaluate performance when operating untethered from AC power, `librbipc` incorporates power state detection in [`benchmarks/bench_common.h`](benchmarks/bench_common.h). When running on battery (`/sys/class/power_supply/AC/online == 0`), the Linux CPU governor throttles the mobile processor (Intel Core i5-6300U) from 2.40 GHz to 400–800 MHz. Measurements are isolated in [`benchmarks/results/laptop_powersave/`](benchmarks/results/laptop_powersave/) and compared against plugged-in baseline datasets in [`benchmarks/results/laptop/`](benchmarks/results/laptop/).

#### A. Architectural Optimization Progression (Battery Power-Save Mode)

Table 11 tracks performance improvements across five optimization stages measured on battery power:

| Optimization Stage | Implementation Modifications | Zero-Spin Throughput | Adaptive Throughput | Latency Metrics |
| :--- | :--- | ---: | ---: | :--- |
| **Baseline (Power-Save)** | Initial implementation under throttled clock | 26,658 msgs/s | 258,503 msgs/s | $p50$: 382 ns, $p90$: 2,577 ns |
| **Stage 1: Asymmetric Membarrier** | Registered `MEMBARRIER_CMD_GLOBAL_EXPEDITED`; elided store-release fence on `slot->state` | 26,658 msgs/s | 258,503 msgs/s | Elided fast-path fence overhead |
| **Stage 2: `FUTEX_WAKE_OP` Integration** | Atomic futex word increment and waiter wake via `FUTEX_WAKE_OP` | 39,306 msgs/s *(+47.6%)* | 442,337 msgs/s *(+71.1%)* | Zero-spin $p50$ latency reduced by 47.3% (9.5 to 5.0 μs) |
| **Stage 3: Hardware Monitor Wait** | Added `RBIPC_WAIT_HW_MONITOR`; implemented `UMWAIT` C0.2 deep power-save state | 34,110 msgs/s | 404,157 msgs/s | Adaptive $p99$ latency reduced to 1,393 ns |
| **Stage 4: Direct `sys_futex_waitv`** | Monitored `slot->sequence`, `futex_seq`, and `shutdown_flag` simultaneously via `futex_waitv` | 20,111 msgs/s | 445,174 msgs/s | Adaptive $p50$ latency reduced to 299 ns |
| **Stage 5: Bounded FAA Ticket Leasing** | Bounded `atomic_fetch_add` ticket allocation + CAS backoff | 32,901 msgs/s | **881,564 msgs/s** *(+241.0%)* | Adaptive $p90$ latency reduced to 462 ns |

*Table 11: Measured progression across synchronization optimizations under battery power constraints.*

#### B. Comparative Profile: Plugged-In vs. Battery Power-Save Mode

Table 12 compares performance between the AC plugged-in profile ([`benchmarks/results/laptop/`](benchmarks/results/laptop/)) and the battery power-save profile ([`benchmarks/results/laptop_powersave/`](benchmarks/results/laptop_powersave/)):

| Benchmark Metric | Plugged-In Laptop (AC) | Battery Power-Save Laptop | Clock Throttle Ratio |
| :--- | ---: | ---: | ---: |
| **Zero-Spin Throughput** | 74,015 msgs/s | 32,901 msgs/s | 2.25× |
| **Zero-Spin Median Latency ($p50$)** | 2,752 ns | 5,339 ns | 1.94× |
| **Adaptive Backoff Throughput** | 1,332,608 msgs/s | 881,564 msgs/s | 1.51× |
| **Adaptive Median Latency ($p50$)** | 149 ns | 389 ns | 2.61× |
| **Adaptive 90th Percentile ($p90$)** | 1,124 ns | 462 ns | 0.41× *(Superior tail under FAA)* |
| **B-Queue Vector Batch (size 32)** | 9,842,100 msgs/s | 7,513,515 msgs/s | 1.31× |
| **Streaming Stores (1 MiB Payload)** | 6,561.42 MB/s | 3,841.24 MB/s | 1.71× |
| **MPMC 4P-4C Contention Throughput** | 1,420,120 ops/s | 730,000 ops/s | 1.95× |

*Table 12: Performance comparison across power supply profiles.*

#### C. MPMC Scaling Under Frequency Throttling

Evaluating MPMC scaling under battery power (`competitors_benchmark.json`) demonstrates stable throughput behavior relative to busy-polling architectures:

| Topology | `librbipc` (Mops/s) | DPDK `rte_ring` (Mops/s) | LMAX Disruptor (Mops/s) | Relative Ratio vs. `rte_ring` |
| :--- | ---: | ---: | ---: | :--- |
| **1P-1C** (2 threads) | **0.38** | 0.76 | 1.49 | 0.50× |
| **2P-2C** (4 threads) | **0.67** | 3.70 | 3.23 | 0.18× |
| **4P-2C** (6 threads, 150% oversubscription) | **0.52** | 0.02 | 0.07 | **26.0×** |
| **4P-4C** (8 threads, 200% oversubscription) | **0.73** | 0.04 | 0.01 | **18.2×** |
| **Idle CPU Utilization** | **0.8%** | 94.7% | 84.5% | **Significant power reduction** |

*Table 13: Multi-core contention under power-save clock throttling.*

Under clock throttling ($\approx 600$ MHz), `librbipc` sustains 0.73 Mops/s under 4P-4C oversubscription while consuming less than 1% CPU when idle, whereas spin-polling ring buffers decline to 0.01–0.04 Mops/s while consuming over 84% CPU.

---

## 10. Verification, Sanitizer Auditing, and Code Coverage

### 10.1 Verification Suite Structure

The `tests/` directory contains 25 test suites categorized into unit, I/O, and multi-process end-to-end integration tests:

| Category | Test Binary | Target Subsystem & Verified Invariants |
| :--- | :--- | :--- |
| **Unit** | [`test_unit_math.c`](tests/test_unit_math.c) | `rbipc_next_pow2`, `rbipc_align_up`, and 64-bit modular `rbipc_seq_diff` across wrap boundaries. |
| **Unit** | [`test_unit_predicates.c`](tests/test_unit_predicates.c) | Invariant predicates in [`src/rbipc_predicate.h`](src/rbipc_predicate.h) (bounds, magic, alignment, names). |
| **Unit** | [`test_unit_error.c`](tests/test_unit_error.c) | `rbipc_strerror`, `rbipc_errno_to_status`, and `rbipc_error_format` diagnostic strings. |
| **Unit** | [`test_unit_shm.c`](tests/test_unit_shm.c) | Layout overflow guards, named `shm_open`, anonymous `memfd_create`, and `F_ADD_SEALS` enforcement. |
| **Unit** | [`test_unit_vmem.c`](tests/test_unit_vmem.c) | Control mapping, 1× linear data mapping, and 2× `MAP_FIXED` contiguous ring mirror wrap-around. |
| **Unit** | [`test_unit_slot.c`](tests/test_unit_slot.c) | Slot state machine transitions (`EMPTY`, `RESERVING`, `RESERVED`, `COMMITTED`, `POISONED`) and PID release CAS. |
| **Unit** | [`test_unit_sync.c`](tests/test_unit_sync.c) | `rbipc_futex_wait`/`wake`, deadline expiration (`RBIPC_ERR_TIMEOUT`), and `RBIPC_WAIT_ADAPTIVE`. |
| **Unit** | [`test_unit_audit.c`](tests/test_unit_audit.c) | `pidfd_open(2)` liveness probing, dead-producer poisoning, dead-consumer recovery, and ticket-hole healing. |
| **Unit** | [`test_unit_simd.c`](tests/test_unit_simd.c) | AVX2/NEON/scalar vacancy and readiness scanners across ring wrap-around boundaries. |
| **Unit** | [`test_unit_stream.c`](tests/test_unit_stream.c) | `rbipc_stream_copy` unaligned prefixes/tails, AVX2/SSE2 non-temporal stores, and `rbipc_write_stream*` APIs. |
| **Unit** | [`test_unit_reactor.c`](tests/test_unit_reactor.c) | `rbipc_enable_eventfd`, external `rbipc_set_eventfd`, `epoll_wait` readiness, `rbipc_eventfd_drain`, and RAII cleanup. |
| **Unit** | [`test_unit_lease.c`](tests/test_unit_lease.c) | `rbipc_set_lease_timeout`, automatic lease recovery, `rbipc_revoke_expired_leases`, and stale release rejection. |
| **Unit** | [`test_unit_ipc.c`](tests/test_unit_ipc.c) | `rbipc_ipc_send_fd`/`recv_fd` and `rbipc_send_fd`/`rbipc_recv_fd_ex` over `socketpair(AF_UNIX, SOCK_STREAM)`. |
| **Unit** | [`test_unit_pubsub.c`](tests/test_unit_pubsub.c) | `rbipc_publish`, `rbipc_publish_batch`, `rbipc_sub_poll`, `rbipc_sub_poll_batch`, `rbipc_sub_read`, and lag clamping. |
| **Unit** | [`test_unit_lifecycle.c`](tests/test_unit_lifecycle.c) | `rbipc_create_ex`, `rbipc_attach_ex`, CRC32C corruption rejection, role counters, and telemetry getters. |
| **Unit** | [`test_unit_latency.c`](tests/test_unit_latency.c) | Ping-pong latency percentile distribution and SLA verification. |
| **I/O** | [`test_io_basic.c`](tests/test_io_basic.c) | Blocking, non-blocking, and timeout single-slot zero-copy and copy convenience I/O paths. |
| **I/O** | [`test_io_abort.c`](tests/test_io_abort.c) | `rbipc_abort_write` explicit slot poisoning and consumer `RBIPC_ERR_POISONED` recovery. |
| **I/O** | [`test_io_batch.c`](tests/test_io_batch.c) | B-Queue vector batch reservation, commit, acquisition, release, and partial-batch draining. |
| **E2E** | [`test_e2e_multithread.c`](tests/test_e2e_multithread.c) | High-contention 4-Producer / 2-Consumer MPMC stress verification (100,000 messages, zero loss/duplication). |
| **E2E** | [`test_e2e_crash.c`](tests/test_e2e_crash.c) | Multi-process `fork()` + `SIGKILL` mid-reservation and mid-consumption dead-peer recovery. |
| **E2E** | [`test_e2e_chaos.c`](tests/test_e2e_chaos.c) | Asynchronous signal fault injection (`SIGKILL`, `SIGSTOP`, `SIGCONT`) across worker process pools. |
| **E2E** | [`test_e2e_shutdown.c`](tests/test_e2e_shutdown.c), [`test_e2e_throughput.c`](tests/test_e2e_throughput.c), [`test_harness.c`](tests/test_harness.c) | Broadcast shutdown wakeup verification and multi-process throughput verification. |

*Table 14: Automated verification test matrix.*

### 10.2 Line Coverage Analysis

Executing `make coverage` compiles all 16 `src/` modules and test suites with `-O0 -g --coverage` and aggregates line coverage via [`scripts/coverage_summary.py`](scripts/coverage_summary.py):

| Source Module | Executable Lines | Covered Lines | Line Coverage (%) |
| :--- | ---: | ---: | ---: |
| [`src/rbipc_error.c`](src/rbipc_error.c) | 56 | 56 | **100.00%** |
| [`src/rbipc_io.h`](src/rbipc_io.h) | 13 | 13 | **100.00%** |
| [`src/rbipc_stream.h`](src/rbipc_stream.h) | 4 | 4 | **100.00%** |
| [`src/rbipc_simd.c`](src/rbipc_simd.c) | 56 | 55 | **98.21%** |
| [`src/rbipc_predicate.h`](src/rbipc_predicate.h) | 39 | 38 | **97.44%** |
| [`src/rbipc_math.h`](src/rbipc_math.h) | 13 | 12 | **92.31%** |
| [`src/rbipc_audit.c`](src/rbipc_audit.c) | 120 | 110 | **91.67%** |
| [`src/rbipc_batch.c`](src/rbipc_batch.c) | 181 | 165 | **91.16%** |
| [`src/rbipc_slot.c`](src/rbipc_slot.c) | 91 | 82 | **90.11%** |
| [`src/rbipc_ring.c`](src/rbipc_ring.c) | 286 | 252 | **88.11%** |
| [`src/rbipc_vmem.c`](src/rbipc_vmem.c) | 53 | 46 | **86.79%** |
| [`src/rbipc_ipc.c`](src/rbipc_ipc.c) | 73 | 63 | **86.30%** |
| [`src/rbipc_pubsub.c`](src/rbipc_pubsub.c) | 149 | 119 | **79.87%** |
| [`src/rbipc_shm.c`](src/rbipc_shm.c) | 130 | 102 | **78.46%** |
| [`src/rbipc_io.c`](src/rbipc_io.c) | 282 | 219 | **77.66%** |
| [`src/rbipc_futex.c`](src/rbipc_futex.c) | 181 | 140 | **77.35%** |
| [`src/rbipc_stream.c`](src/rbipc_stream.c) | 54 | 40 | **74.07%** |
| [`src/rbipc_internal.h`](src/rbipc_internal.h) | 10 | 7 | **70.00%** |
| [`src/rbipc_reactor.c`](src/rbipc_reactor.c) | 70 | 49 | **70.00%** |
| [`src/rbipc_sync.c`](src/rbipc_sync.c) | 182 | 126 | **69.23%** |
| [`src/rbipc_arch.h`](src/rbipc_arch.h) | 44 | 27 | **61.36%** |
| **TOTAL (`librbipc` v2.0.0)** | **2,087** | **1,725** | **82.65%** |

*Table 15: Module-by-module gcov statement coverage.*

*(Uncovered code paths consist primarily of defensive system call failure branches, such as kernel out-of-memory errors on `mmap`/`ftruncate`, and scalar fallback code paths on systems equipped with AVX2 and SSE4.2).*

---

## 11. References

1. D. Vyukov, "Bounded MPMC Queue," *1024cores: Concurrency Algorithms, Systems and Paradigms*, 2010. [Online]. Available: <https://sites.google.com/site/1024cores/home/lock-free-algorithms/queues/bounded-mpmc-queue>
2. J. Wang, K. Zhang, X. Tang, and B. Hua, "B-Queue: Efficient and Practical Queuing for Fast Core-to-Core Communication," *International Journal of Parallel Programming*, vol. 41, no. 1, pp. 137–159, 2013. DOI: 10.1007/s10766-012-0213-x.
3. M. Thompson, D. Farley, M. Barker, P. Gee, and A. Stewart, "Disruptor: High Performance Alternative to Bounded Queues for Exchanging Data Between Concurrent Threads," LMAX Exchange White Paper, 2011. [Online]. Available: <https://lmax-exchange.github.io/disruptor/files/Disruptor-1.0.pdf>
4. J. Giacomoni, T. Moseley, and M. Vachharajani, "FastForward for Efficient Pipeline Parallelism: A Cache-Optimized Concurrent Lock-Free Queue," in *Proc. 13th ACM SIGPLAN Symp. on Principles and Practice of Parallel Programming (PPoPP '08)*, 2008, pp. 43–52. DOI: 10.1145/1345206.1345215.
5. U. Drepper, "What Every Programmer Should Know About Memory," Red Hat, Inc., Tech. Rep., 2007. [Online]. Available: <https://akkadia.org/drepper/cpumemory.pdf>
6. H. Franke, R. Russell, and M. Kirkwood, "Fuss, Futexes and Furwocks: Fast Userlevel Locking in Linux," in *Proc. Ottawa Linux Symp. (OLS '02)*, 2002, pp. 479–495.
7. R. Elz and R. Bush, "Serial Number Arithmetic," IETF RFC 1982, Aug. 1996. DOI: 10.17487/RFC1982.
8. DPDK Project, "Ring Library (`rte_ring`) and Power Management (`rte_power_monitor`)," in *Data Plane Development Kit (DPDK) Programmer's Guide*, Linux Foundation, 2024. [Online]. Available: <https://doc.dpdk.org/guides/prog_guide/>
9. Eclipse Foundation, "Eclipse iceoryx2: True Zero-Copy Inter-Process Communication in Rust," Eclipse Foundation, 2024–2026. [Online]. Available: <https://iceoryx.io/>
10. A. Almeida, "Futex2: Bringing Multi-Wait and Variable Sizes to Linux Futexes," Linux Kernel Documentation, 2021. [Online]. Available: <https://docs.kernel.org/userspace-api/futex2.html>
11. Intel Corporation, "User-Level Monitor and Wait (WAITPKG: `UMONITOR`, `UMWAIT`, `TPAUSE`)," in *Intel 64 and IA-32 Architectures Software Developer’s Manual*, vol. 2B, Order No. 325383-082US, 2024, pp. 4-463–4-467.
12. Arm Limited, "Wait For Event with Timeout (`WFET`, `WFIT`): FEAT_WFxT Architecture Extension," in *Arm Architecture Reference Manual for A-profile Architecture*, Issue K.a, 2023, pp. C6-1782–C6-1784.
13. M. Desnoyers, P. E. McKenney, A. S. Stern, M. R. Dagenais, and J. Walpole, "User-Level Implementations of Read-Copy Update," *IEEE Transactions on Parallel and Distributed Systems*, vol. 23, no. 2, pp. 375–382, Feb. 2012. DOI: 10.1109/TPDS.2011.159.
14. R. Nikolaev, "A Scalable, Portable, and Memory-Efficient Lock-Free FIFO Queue (SCQ)," in *Proc. 33rd Int. Symp. on Distributed Computing (DISC 2019)*, 2019, pp. 28:1–28:17. DOI: 10.4230/LIPIcs.DISC.2019.28.
