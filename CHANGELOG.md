# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- Real Intel DSA MOVDIR64B dispatch in the DMA write path. The DMA engine now
  builds a 64-byte hardware descriptor, submits it to a work-queue portal via
  `MOVDIR64B`, and polls the completion record, falling back to the
  non-temporal streaming pipeline when DSA is unavailable or submission fails.
- Wait-free producer post wired into the blocking write path: when the ring is
  full, the producer posts a READ help request for the oldest unread slot
  before sleeping, reducing wake latency for wait-free consumers.

### Changed
- Doxyfile version now derives from the Makefile `VERSION` single source of
  truth instead of a hardcoded value.
- Dropped the unnecessary `-lrt` from `LDFLAGS` (glibc >= 2.17 provides
  `clock_gettime`/`shm_open` in libc).
- `gen_compile_commands.py` now emits complete entries for multi-source
  benchmark binaries.

### Fixed
- `ARCHITECTURE.md` header/slot layout drift vs the actual ABI.
- `README.md` automated test suite count (31 -> 38).
- `BENCHMARKS.md` data inconsistency between "Trusted 8B Pointer" and
  "Combined Mode" rows.
- Redundant `slot->state` store in the packed-mode batch commit path.

## [1.0.0] - 2026-11-05

### Added
- Lock-free, zero-dependency shared-memory ring buffer IPC library in ISO C23.
- Single-slot zero-copy I/O (`rbipc_reserve_write` / `rbipc_commit_write` /
  `rbipc_read`).
- B-Queue vector batching with software prefetching and futex waiter
  elimination.
- Zero-spin passive IPC with Linux futex and kernel bottleneck elimination.
- MPMC multi-producer/multi-consumer contention scalability.
- Pub/Sub multicast with per-subscriber lag tracking.
- Hardware-assisted DMA offload engine (Intel DSA) with streaming fallback.
- Memory protection keys (MPK) and memory sealing (mseal) isolation.
- CXL memory support with capability probing.
- UINTR (user interrupts), io_uring, and eventfd integration.
- Comprehensive test suite (38 binaries) and benchmark suite.

[Unreleased]: https://github.com/Seintian/librbipc/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/Seintian/librbipc/releases/tag/v1.0.0