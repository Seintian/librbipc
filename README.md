# librbipc

**librbipc** is a production-grade, high-performance, lock-free, zero-copy inter-process communication (IPC) ring buffer library written in Modern C (C11/C23) for Linux.

Designed following strict Software Engineering principles (Single Responsibility Principle, high cohesion, low coupling, defensive programming), **librbipc** delivers sub-microsecond latency, multi-million message throughput across distinct operating system processes, dead-peer crash recovery, and comprehensive test coverage.

---

## Architectural Principles & Highlights

1. **Virtual Memory Double-Mapping Mirror Window**:
   - Consecutive `mmap` calls with `MAP_FIXED` map a single underlying shared memory data region twice into contiguous virtual memory.
   - Any slice of size up to buffer capacity straddling the circular ring boundary is contiguously accessible without split reads/writes.
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
   - Producers head (`write_ticket`), consumer tail (`read_ticket`), and per-slot descriptors are strictly 64-byte aligned (`_Alignas(64)`), eliminating cache ping-pong across CPU cores.
6. **3-Tier Adaptive Hybrid Backoff**:
   - **Tier 1 (Spin)**: Low-latency busy-wait with CPU pause hints (`_mm_pause` / `isb`).
   - **Tier 2 (Yield)**: Cooperative OS scheduling yield via `sched_yield()`.
   - **Tier 3 (Futex)**: Process-shared Linux `sys_futex` kernel wait (`FUTEX_WAIT` / `FUTEX_WAKE`) to drop idle CPU utilization to 0%.
7. **Linux File Sealing & Memfd Inheritance**:
   - Descriptors are sealed via `F_ADD_SEALS` (`F_SEAL_SHRINK | F_SEAL_GROW`) against accidental truncation `SIGBUS` panics.
   - Direct file descriptor attachment (`rbipc_attach_fd`) enables anonymous `memfd_create` buffers to be passed over UNIX domain sockets (`SCM_RIGHTS`).

---

## Modular Architecture

```
librbipc/
├── include/
│   └── rbipc.h                 # Clean, stable public API & types
├── src/
│   ├── rbipc_arch.h            # Architecture CPU pause & monotonic timing
│   ├── rbipc_math.h            # Overflow-safe integer & sequence arithmetic
│   ├── rbipc_futex.h/.c        # Linux sys_futex kernel wrappers
│   ├── rbipc_shm.h/.c          # Shared memory lifecycle, sizing & sealing
│   ├── rbipc_vmem.h/.c         # Virtual memory mapping & mirror trick
│   ├── rbipc_slot.h/.c         # Slot state machine & dead-peer recovery
│   ├── rbipc_sync.h/.c         # 3-tier hybrid backoff engine & wakeups
│   ├── rbipc_ring.c            # Ring lifecycle, attach_fd, stats & shutdown
│   ├── rbipc_io.c              # Zero-copy reserve, commit, abort, acquire & release
│   └── rbipc_error.c           # Error description strings (rbipc_strerror)
├── tests/
│   ├── test_unit_math.c        # Power-of-2 rounding, alignment & sequence wrap-around
│   ├── test_unit_error.c       # Error string code validation
│   ├── test_unit_shm.c         # Layout calculations, file sealing & descriptor limits
│   ├── test_unit_vmem.c        # Double-mapping mirror verification & boundary wrapping
│   ├── test_unit_slot.c        # Slot transitions, sequence turns & peer liveness
│   ├── test_unit_sync.c        # Hybrid backoff timeouts & futex wakeups
│   ├── test_unit_lifecycle.c   # Parameter validation, corruption defense & stats
│   ├── test_io_basic.c         # Single-process I/O, non-blocking & timeout tests
│   ├── test_io_abort.c         # Write reservation abort & recovery
│   ├── test_e2e_shutdown.c     # Shutdown wakeup of blocked producers/consumers
│   ├── test_e2e_crash.c        # Process SIGKILL dead-peer crash recovery
│   ├── test_e2e_multithread.c  # MPMC concurrent thread contention stress test
│   ├── test_e2e_throughput.c   # Multi-process fork throughput & latency benchmark
│   └── test_harness.c          # Master verification harness
├── scripts/
│   ├── gen_compile_commands.py # Generates compile_commands.json for clangd/LSP
│   └── coverage_summary.py     # Parses gcov metrics and prints summary table
├── Makefile                    # Warning-free builds, tests, coverage, valgrind, clang-tidy
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

# Run full 14-binary unit, integration, and stress test suite
make test

# Generate code coverage report via gcov
make coverage

# Verify memory safety with zero leaks under Valgrind
make valgrind

# Run Clang-Tidy static analysis
make clang-tidy
```

### Test Coverage Summary

```
================================================================================
Source File                    | Executable Lines | Covered Lines  | Coverage %
================================================================================
rbipc_arch.h                   | 5                | 5              |   100.00%
rbipc_error.c                  | 26               | 26             |   100.00%
rbipc_futex.c                  | 16               | 15             |    93.75%
rbipc_io.c                     | 112              | 112            |   100.00%
rbipc_math.h                   | 14               | 14             |   100.00%
rbipc_ring.c                   | 174              | 155            |    89.08%
rbipc_shm.c                    | 76               | 64             |    84.21%
rbipc_slot.c                   | 32               | 32             |   100.00%
rbipc_sync.c                   | 44               | 43             |    97.73%
rbipc_vmem.c                   | 32               | 27             |    84.38%
================================================================================
TOTAL LINE COVERAGE            | 531              | 493            |    92.84%
================================================================================
```

---

## License

MIT License / Apache-2.0.
