# librbipc

**librbipc** is a production-grade, high-performance, lock-free, zero-copy inter-process communication (IPC) ring buffer library written in Modern C (ISO C23 / C11) for Linux.

Designed following strict Software Engineering principles (Single Responsibility Principle, high cohesion, low coupling, defensive programming, and formal domain predicates), **librbipc** delivers sub-microsecond latency, multi-million message throughput across distinct operating system processes, dead-peer crash recovery, exhaustive Doxygen API documentation, and comprehensive test coverage.

---

## Architectural Principles & Highlights

1. **Virtual Memory Double-Mapping Mirror Window**:
   - Consecutive `mmap` calls with `MAP_FIXED` map a single underlying shared memory data region twice into contiguous virtual memory.
   - Any slice of size up to buffer capacity straddling the circular ring boundary is contiguously accessible without split reads/writes or intermediate buffering.
2. **True Zero-Copy Protocol**:
   - Producers loan cache-aligned shared memory slots (`rbipc_reserve_write`) and publish them in-place (`rbipc_commit_write`).
   - Consumers acquire direct memory pointers to committed payloads without memory copying (`rbipc_read_acquire`).
3. **Dead-Peer Crash Recovery & Write Abort**:
   - Each slot descriptor tracks PID and state machine (`EMPTY`, `RESERVED`, `COMMITTED`, `POISONED`).
   - If a producer process crashes or aborts (`rbipc_abort_write`), consumers detect `ESRCH` via `kill(pid, 0)`, atomically transition the slot to `POISONED`, and advance the pipeline to prevent deadlocks.
4. **Resilient Timeouts & Non-Blocking Primitives**:
   - Non-blocking variants (`rbipc_reserve_write_nonblock`, `rbipc_read_acquire_nonblock`) return `RBIPC_ERR_FULL` / `RBIPC_ERR_EMPTY` immediately.
   - Nanosecond-precision timeout variants (`rbipc_reserve_write_timeout`, `rbipc_read_acquire_timeout`) guard mission-critical systems against indefinite hangs.
5. **Cache-Line Isolated Synchronization (No False Sharing)**:
   - Producer head (`write_ticket`), consumer tail (`read_ticket`), and per-slot descriptors are strictly 64-byte aligned (`_Alignas(64)`), eliminating cache ping-pong across CPU cores.
6. **Zero-Spin Passive Waiting Synchronization**:
   - Zero active waiting: eliminates CPU spinning and busy-wait loops (`_mm_pause` / `sched_yield`) completely.
   - Immediate passive suspension via process-shared Linux `sys_futex` kernel wait (`FUTEX_WAIT` / `FUTEX_WAKE`) to drop idle CPU utilization to strictly 0.08%.
7. **Kernel Syscall Elimination & Asymmetric Channels**:
   - Asymmetric synchronization channels (`read_futex_seq` for consumers, `write_futex_seq` for producers) isolate wakeups and eliminate cross-channel false wakeups.
   - Waiter tracking via atomic counters (`futex_waiters`, `write_waiters`). On hot streaming paths where peers are active, producers and consumers completely bypass `sys_futex` syscalls, achieving sub-microsecond in-memory operations.
8. **Software Prefetching & Compiler Optimizations**:
   - Prefetches upcoming slot control descriptors into L1/L2 cache and payload data buffers ahead of the CPU pipeline (`__builtin_prefetch`), hiding memory access latency.
   - Heavily annotated with modern compiler attributes (`RBIPC_NODISCARD`, `RBIPC_INLINE`, `RBIPC_PURE`, `RBIPC_CONST`, `RBIPC_LEAF`, `RBIPC_ASSUME_ALIGNED`, and `restrict`), guiding aggressive compiler loop unrolling, instruction scheduling, and vectorization.
9. **B-Queue Batching API (PPoPP '08 / IJPP '13)**:
   - Vector-based burst operations (`rbipc_reserve_write_batch`, `rbipc_commit_write_batch`, `rbipc_read_acquire_batch`, `rbipc_read_release_batch`) amortize atomic CAS overhead over $N$ items in a single CAS, scaling throughput past 38,000,000 msgs/sec.
10. **Linux File Sealing & Memfd Inheritance**:
    - Descriptors are sealed via `F_ADD_SEALS` (`F_SEAL_SHRINK | F_SEAL_GROW`) against accidental truncation `SIGBUS` panics.
    - Direct file descriptor attachment (`rbipc_attach_fd`) enables anonymous `memfd_create` buffers to be passed over UNIX domain sockets (`SCM_RIGHTS`).

---

## Modular Architecture

```
librbipc/
├── include/
│   └── rbipc.h                 # Clean, stable public API, types & compiler annotations
├── src/
│   ├── rbipc_attr.h            # Compiler optimization attributes, hints & annotations
│   ├── rbipc_arch.h            # Architecture CPU pause & monotonic clock timing
│   ├── rbipc_math.h            # Power-of-2 rounding, alignment & RFC 1982 modular arithmetic
│   ├── rbipc_predicate.h       # Mathematical domain predicates & state verification
│   ├── rbipc_internal.h        # Concrete ring structure & internal invariants
│   ├── rbipc_error.h/.c        # Error classification & diagnostic descriptions
│   ├── rbipc_futex.h/.c        # Linux sys_futex kernel wrappers
│   ├── rbipc_slot.h/.c         # Slot state machine & dead-peer crash recovery
│   ├── rbipc_sync.h/.c         # Adaptive spin-then-wait backoff engine & futex wakeups
│   ├── rbipc_shm.h/.c          # Shared memory lifecycle, geometry sizing & file sealing
│   ├── rbipc_vmem.h/.c         # Virtual memory mapping abstractions & double-mapped mirror
│   ├── rbipc_ring.h/.c         # Ring lifecycle, handle management, stats & shutdown
│   └── rbipc_io.h/.c           # Zero-copy reserve, commit, abort, acquire & batching pipeline
├── tests/
│   ├── test_unit_predicates.c  # Domain predicate correctness & boundary validation
│   ├── test_unit_math.c        # Power-of-2 rounding, alignment & sequence wrap-around
│   ├── test_unit_error.c       # Error string code validation
│   ├── test_unit_shm.c         # Layout calculations, file sealing & descriptor limits
│   ├── test_unit_vmem.c        # Double-mapping mirror verification & boundary wrapping
│   ├── test_unit_slot.c        # Slot transitions, sequence turns & peer liveness
│   ├── test_unit_sync.c        # Hybrid backoff timeouts & futex wakeups
│   ├── test_unit_lifecycle.c   # Parameter validation, corruption defense & stats
│   ├── test_io_basic.c         # Single-process I/O, non-blocking & timeout tests
│   ├── test_io_abort.c         # Write reservation abort & recovery
│   ├── test_io_batch.c         # B-Queue vector batching (burst operations)
│   ├── test_e2e_shutdown.c     # Shutdown wakeup of blocked producers/consumers
│   ├── test_e2e_crash.c        # Process SIGKILL dead-peer crash recovery
│   ├── test_e2e_multithread.c  # MPMC concurrent thread contention stress test
│   ├── test_e2e_throughput.c   # Multi-process fork throughput & latency benchmark
│   └── test_harness.c          # Master verification harness
├── benchmarks/
│   ├── bench_suite.c           # Comprehensive benchmark harness (throughput, latency, batching, payload)
│   └── results/                # Versioned baseline and zero-spin JSON benchmark telemetry
├── scripts/
│   ├── gen_compile_commands.py # Generates compile_commands.json for clangd/LSP
│   └── coverage_summary.py     # Parses gcov metrics and prints summary table
├── Makefile                    # Warning-free builds, tests, benchmarks, coverage, valgrind, clang-tidy, docs
├── Doxyfile                    # Doxygen API configuration
├── BENCHMARK_REPORT.md         # In-depth perf profiling & benchmark analysis
├── BENCHMARK_ICEORYX2_COMPARISON.md # Metric-by-metric comparison against Eclipse iceoryx2
└── README.md
```

---

## API Overview

```c
#include "rbipc.h"

// Lifecycle & Attachment
int rbipc_create(const char *name, size_t capacity, uint32_t slot_size, rbipc_ring_t **out_ring);
int rbipc_attach(const char *name, rbipc_ring_t **out_ring);
int rbipc_attach_fd(int fd, rbipc_ring_t **out_ring);
int rbipc_detach(rbipc_ring_t *ring);
int rbipc_destroy(const char *name);

// Producer Operations (Zero-Copy)
int rbipc_reserve_write(rbipc_ring_t *ring, uint32_t len, void **out_buf, uint32_t *ticket);
int rbipc_reserve_write_timeout(rbipc_ring_t *ring, uint32_t len, uint64_t timeout_ns, void **out_buf, uint32_t *ticket);
int rbipc_reserve_write_nonblock(rbipc_ring_t *ring, uint32_t len, void **out_buf, uint32_t *ticket);
int rbipc_commit_write(rbipc_ring_t *ring, uint32_t ticket, uint32_t written_len);
int rbipc_abort_write(rbipc_ring_t *ring, uint32_t ticket);

// Consumer Operations (Zero-Copy)
int rbipc_read_acquire(rbipc_ring_t *ring, const void **out_buf, uint32_t *out_len, uint32_t *ticket);
int rbipc_read_acquire_timeout(rbipc_ring_t *ring, uint64_t timeout_ns, const void **out_buf, uint32_t *out_len, uint32_t *ticket);
int rbipc_read_acquire_nonblock(rbipc_ring_t *ring, const void **out_buf, uint32_t *out_len, uint32_t *ticket);
int rbipc_read_release(rbipc_ring_t *ring, uint32_t ticket);

// B-Queue Batch / Burst Operations (High-Throughput Vector API)
int rbipc_reserve_write_batch(rbipc_ring_t *ring, uint32_t count, rbipc_iovec_t *iovecs, uint32_t *out_reserved);
int rbipc_commit_write_batch(rbipc_ring_t *ring, uint32_t count, const uint32_t *tickets, const uint32_t *lens);
int rbipc_read_acquire_batch(rbipc_ring_t *ring, uint32_t count, rbipc_rovec_t *rovecs, uint32_t *out_acquired);
int rbipc_read_release_batch(rbipc_ring_t *ring, uint32_t count, const uint32_t *tickets);

// Observability & Control
int rbipc_signal_shutdown(rbipc_ring_t *ring);
int rbipc_get_fd(const rbipc_ring_t *ring);
int rbipc_get_stats(const rbipc_ring_t *ring, rbipc_stats_t *out_stats);
const char *rbipc_strerror(int err);
```

---

## Build & Test Commands

```bash
# Build static library, shared library, test suite, and compile_commands.json
make clean && make -j8

# Run full 16-binary unit, integration, and stress test suite
make test

# Run comprehensive benchmark suite
make bench

# Generate code coverage report via gcov
make coverage

# Verify memory safety with zero leaks under Valgrind
make valgrind

# Run Clang-Tidy static analysis
make clang-tidy

# Generate HTML Doxygen documentation
make docs
```

### Test Coverage Summary

```
================================================================================
Source File                    | Executable Lines | Covered Lines  | Coverage %
================================================================================
rbipc_arch.h                   | 2                | 2              |   100.00%
rbipc_error.c                  | 26               | 26             |   100.00%
rbipc_futex.c                  | 19               | 15             |    78.95%
rbipc_internal.h               | 10               | 7              |    70.00%
rbipc_io.c                     | 265              | 256            |    96.60%
rbipc_io.h                     | 13               | 13             |   100.00%
rbipc_math.h                   | 11               | 11             |   100.00%
rbipc_predicate.h              | 28               | 28             |   100.00%
rbipc_ring.c                   | 208              | 183            |    87.98%
rbipc_shm.c                    | 103              | 89             |    86.41%
rbipc_slot.c                   | 35               | 35             |   100.00%
rbipc_sync.c                   | 45               | 43             |    95.56%
rbipc_vmem.c                   | 41               | 35             |    85.37%
================================================================================
TOTAL LINE COVERAGE            | 806              | 743            |    92.18%
================================================================================
```

### Performance Benchmarks (x86_64 Linux, Zero-Spin Passive IPC)

| Mode | Throughput | Bandwidth | Average Latency |
| :--- | :--- | :--- | :--- |
| **Single-Item (Zero-Copy)** | **1,151,240 msgs/sec** | ~73.7 MB/sec | **868 ns** |
| **Streaming (Hot Cache)** | **13,733,522 msgs/sec** | **838.2 MB/sec** | **72.8 ns** |
| **B-Queue Vector (Burst 128)** | **38,111,878 msgs/sec** | **2,326.2 MB/sec** | **26.2 ns** |
| **Large Payload (64 KB)** | **4,396,079 msgs/sec** | **274.8 GB/sec** | **227 ns** |

*For complete profiling methodology, hardware counter analysis, and `perf` breakdown, see [BENCHMARK_REPORT.md](BENCHMARK_REPORT.md).*  
*For an exhaustive metric-by-metric comparison with Eclipse `iceoryx2`, see [BENCHMARK_ICEORYX2_COMPARISON.md](BENCHMARK_ICEORYX2_COMPARISON.md).*

---

## License

[Apache-2.0](LICENSE).
