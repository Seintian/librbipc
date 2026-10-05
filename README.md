# librbipc

**librbipc** is a high-performance, lock-free, zero-copy inter-process communication (IPC) ring buffer library written in Modern C (C11/C23) for Linux.

It is designed for mission-critical, low-latency applications requiring sub-microsecond throughput across distinct operating system processes.

---

## Key Features

1. **Double-Mapped Virtual Memory Trick**:
   - Consecutive `mmap` calls with `MAP_FIXED` over a reserved contiguous $2 \times \text{Buffer Size}$ address window backed by a single shared memory descriptor (`shm_open` or `memfd_create`).
   - Slices are contiguously accessible across the circular ring boundary without manual split-read/split-write handling.
2. **True Zero-Copy Architecture**:
   - Payload memory is loaned and committed directly in shared memory (`rbipc_reserve_write` / `rbipc_commit_write`).
   - Consumers acquire direct memory pointers to the committed chunk without copying (`rbipc_read_acquire`).
3. **Cache-Line Isolated Synchronization**:
   - Producer head (`write_ticket`), consumer tail (`read_ticket`), and per-slot descriptors are strictly aligned to 64 bytes (`_Alignas(64)`), completely eliminating CPU False Sharing across cores.
4. **3-Tier Hybrid Backoff**:
   - **Tier 1**: Busy-wait spin loop using hardware CPU pause hints (`_mm_pause()` / `isb`).
   - **Tier 2**: Cooperative thread yielding via `sched_yield()`.
   - **Tier 3**: Process-shared Linux `sys_futex` (`FUTEX_WAIT` / `FUTEX_WAKE`) to drop CPU utilization to 0% when idle.
5. **Dead-Peer Crash Recovery**:
   - Each slot tracks state (`EMPTY`, `RESERVED`, `COMMITTED`, `POISONED`) and producer PID.
   - If a producer process crashes while holding a reserved slot, consumers detect `ESRCH` via `kill(pid, 0)`, atomically transition the slot to `POISONED`, and advance the pipeline to prevent deadlocks.
6. **Linux File Sealing**:
   - Shared memory descriptors are sealed via `fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW)` to prevent `SIGBUS` panics.

---

## Directory Layout

```
librbipc/
├── include/
│   └── rbipc.h           # Public API, strictly aligned types, state machine constants
├── src/
│   └── rbipc.c           # Double-mapping, lock-free ticketing, hybrid backoff, crash detection
├── tests/
│   └── test_harness.c    # End-to-end multi-process verification, latency benchmark, crash recovery
├── Makefile              # Strict warning-free C11 build rules (-Wall -Wextra -Wpedantic -Werror)
├── .gitignore
└── README.md
```

---

## API Summary

```c
#include "rbipc.h"

// Lifecycle
int rbipc_create(const char *name, size_t capacity, uint32_t slot_size, rbipc_ring_t **out_ring);
int rbipc_attach(const char *name, rbipc_ring_t **out_ring);
int rbipc_detach(rbipc_ring_t *ring);
int rbipc_destroy(const char *name);

// Producer (Zero-Copy)
int rbipc_reserve_write(rbipc_ring_t *ring, uint32_t len, void **out_buf, uint32_t *ticket);
int rbipc_commit_write(rbipc_ring_t *ring, uint32_t ticket, uint32_t written_len);

// Consumer (Zero-Copy)
int rbipc_read_acquire(rbipc_ring_t *ring, const void **out_buf, uint32_t *out_len, uint32_t *ticket);
int rbipc_read_release(rbipc_ring_t *ring, uint32_t ticket);

// Control
int rbipc_signal_shutdown(rbipc_ring_t *ring);
int rbipc_get_fd(const rbipc_ring_t *ring);
```

---

## Building & Running Tests

```bash
# Build static library and test harness
make clean && make -j4

# Run test suite
make test
```

### Test Suite Output

```
=== librbipc End-to-End Test Harness ===
Ring buffer created successfully (Capacity: 1024, Slot Size: 256).
[Producer] Starting transmission of 500000 messages...
[Consumer] Listening for messages...

=== Performance Metrics ===
Total Messages : 500000
Elapsed Time   : 1.7304 seconds
Throughput     : 288951.15 msgs/sec (39.68 MB/sec)
Average Latency: 3460.79 ns/message

[Producer] Completed transmission.
Main ordering & throughput test passed successfully!

=== Testing Dead-Peer Crash Recovery ===
[Crash-Test Child] Slot 0 reserved. Crashing abruptly via SIGKILL...
[Crash-Test Parent] Detected child killed. Now attempting consumer acquire...
[Crash-Test Parent] Acquire result: -6 (expected RBIPC_ERR_POISONED = -6)
[Crash-Test Parent] Poisoned slot successfully released and bypassed without deadlock!

All librbipc tests completed and verified successfully!
```

---

## License

MIT License / Apache-2.0.
