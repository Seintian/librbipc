/**
 * @file rbipc.h
 * @brief librbipc: Lock-Free Shared-Memory Ring Buffer Inter-Process Communication
 *
 * @details Implements a bounded, multi-producer multi-consumer (MPMC) lock-free ring buffer
 * architecture situated within POSIX shared memory, targeted at ISO C23 and modern Linux kernels.
 * The library combines three core architectural paradigms:
 * 1. Virtual memory double-mapping (mirror windowing) to present circular ring memory as a
 *    contiguous linear buffer without boundary wraparound splits.
 * 2. Cache-line isolated atomic sequence state machines (ticket-based reservation) operating
 *    with memory order semantics that guarantee single-turn, wait-free coordination on the fast path.
 * 3. Zero-spin passive synchronization utilizing Linux sys_futex primitives to achieve immediate
 *    process suspension during contention or idle intervals, ensuring 0% CPU consumption while
 *    maintaining sub-microsecond resumption latency and automatic dead-peer crash recovery.
 *
 * @author Christian Santarelli
 * @date 2026
 * @copyright Apache License 2.0
 */

#ifndef RBIPC_H
#define RBIPC_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Compiler Directives, Attributes, and Optimization Annotations
 * ============================================================================ */

#if defined(__GNUC__) || defined(__clang__)
/**
 * @def RBIPC_NODISCARD
 * @brief Directs the compiler to emit a warning or error if the function return value is discarded.
 */
#define RBIPC_NODISCARD         __attribute__((warn_unused_result))

/**
 * @def RBIPC_RETURNS_NONNULL
 * @brief Asserts to the compiler that the function will never return a null pointer.
 */
#define RBIPC_RETURNS_NONNULL   __attribute__((returns_nonnull))

/**
 * @def RBIPC_PURE
 * @brief Designates a function whose return value depends solely on its parameters and global memory.
 */
#define RBIPC_PURE              __attribute__((pure))

/**
 * @def RBIPC_CONST
 * @brief Designates a pure mathematical function that examines only its arguments and has no side effects.
 */
#define RBIPC_CONST             __attribute__((const))

/**
 * @def RBIPC_HOT
 * @brief Instructs the compiler that the decorated function is on the execution critical path.
 */
#define RBIPC_HOT               __attribute__((hot))

/**
 * @def RBIPC_COLD
 * @brief Informs the compiler that the function is rarely executed (e.g. error handling, shutdown).
 */
#define RBIPC_COLD              __attribute__((cold))

/**
 * @def RBIPC_LEAF
 * @brief Asserts that the function does not call back into the caller's translation unit.
 * @note Enables global register allocation across translation unit boundaries.
 */
#define RBIPC_LEAF              __attribute__((leaf))

/**
 * @def RBIPC_LIKELY(x)
 * @brief Branch prediction hint indicating that the expression evaluates to true with high probability.
 */
#define RBIPC_LIKELY(x)         (__builtin_expect(!!(x), 1))

/**
 * @def RBIPC_UNLIKELY(x)
 * @brief Branch prediction hint indicating that the expression evaluates to false with high probability.
 */
#define RBIPC_UNLIKELY(x)       (__builtin_expect(!!(x), 0))

/**
 * @def RBIPC_ASSUME_ALIGNED(p, a)
 * @brief Asserts to the optimizer that pointer @p p is aligned to at least @p a bytes.
 */
#define RBIPC_ASSUME_ALIGNED(p, a) __builtin_assume_aligned((p), (a))

/**
 * @def RBIPC_UNREACHABLE()
 * @brief Informs the compiler optimizer that execution cannot reach this point.
 */
#define RBIPC_UNREACHABLE()     __builtin_unreachable()

/**
 * @def RBIPC_RESTRICT
 * @brief C99/C11/C23 restrict keyword indicating disjoint, non-aliasing pointer access.
 */
#define RBIPC_RESTRICT          restrict
#else
#define RBIPC_NODISCARD
#define RBIPC_RETURNS_NONNULL
#define RBIPC_PURE
#define RBIPC_CONST
#define RBIPC_HOT
#define RBIPC_COLD
#define RBIPC_LEAF
#define RBIPC_LIKELY(x)         (x)
#define RBIPC_UNLIKELY(x)       (x)
#define RBIPC_ASSUME_ALIGNED(p, a) (p)
#define RBIPC_UNREACHABLE()     ((void)0)
#define RBIPC_RESTRICT
#endif

/* ============================================================================
 * Constants & Error Codes
 * ============================================================================ */

/**
 * @defgroup rbipc_errors Status & Error Codes
 * @brief Return codes emitted across the librbipc API surface.
 * @{
 */
#define RBIPC_OK               0  /**< Operation completed successfully without error. */
#define RBIPC_ERR_INVAL      (-1) /**< Invalid argument, null pointer, or illegal configuration parameter. */
#define RBIPC_ERR_NOMEM      (-2) /**< Memory allocation or virtual memory mapping subsystem failure. */
#define RBIPC_ERR_SYS        (-3) /**< Underlying Linux kernel system call failure; inspect errno for detail. */
#define RBIPC_ERR_FULL       (-4) /**< Ring buffer is currently full; no write slots available. */
#define RBIPC_ERR_EMPTY      (-5) /**< Ring buffer is currently empty; no committed read messages available. */
#define RBIPC_ERR_POISONED   (-6) /**< Reserving producer terminated prematurely; slot safely poisoned and bypassed. */
#define RBIPC_ERR_SHUTDOWN   (-7) /**< Ring buffer has signaled cooperative termination and was cleanly drained. */
#define RBIPC_ERR_TIMEOUT    (-8) /**< Specified operation deadline expired prior to completion. */
#define RBIPC_ERR_BUSY       (-9) /**< Resource contention or uncommitted slot state encountered. */
#define RBIPC_ERR_OVERFLOW  (-10) /**< Geometric arithmetic or buffer size calculation exceeded addressable range. */
/** @} */

/**
 * @defgroup rbipc_constants Structural Constants
 * @brief Architecture-specific and structural constants.
 * @{
 */
#define RBIPC_MAGIC         0x5242495043323032ULL /**< 64-bit structural header identifier ('RBIPC202'). */
#define RBIPC_VERSION       1                     /**< Protocol layout format version number. */
#define RBIPC_CACHE_LINE    64                    /**< Host hardware cache-line boundary in bytes (x86_64 / arm64). */
/** @} */

/* ============================================================================
 * State Machine Constants
 * ============================================================================ */

/**
 * @enum rbipc_slot_state
 * @brief Lifecycle progression states of a ring buffer slot descriptor.
 */
enum rbipc_slot_state {
    RBIPC_SLOT_EMPTY     = 0, /**< Slot is vacant and available for write ticket reservation. */
    RBIPC_SLOT_RESERVED  = 1, /**< Slot is reserved by a producer; active in-place write payload population. */
    RBIPC_SLOT_COMMITTED = 2, /**< Slot payload write is committed; visible and available for reader consumption. */
    RBIPC_SLOT_POISONED  = 3  /**< Reserving producer crashed or aborted write; slot poisoned to enable reader skip. */
};

/* ============================================================================
 * Shared Memory Data Structures
 * ============================================================================ */

/**
 * @struct rbipc_slot_t
 * @brief Cache-line aligned slot control descriptor residing in shared memory.
 * @details Every slot descriptor manages a single message container within the ring buffer.
 * It is aligned to a 64-byte boundary to prevent false sharing between concurrent worker cores.
 */
typedef struct {
    _Alignas(RBIPC_CACHE_LINE) _Atomic uint32_t sequence;    /**< Monotonically increasing sequence number for turn scheduling. */
    _Atomic uint32_t state;                                  /**< Slot lifecycle state, represented by @ref rbipc_slot_state. */
    _Atomic uint32_t producer_pid;                           /**< Process ID of the reserving producer (for ESRCH crash detection). */
    _Atomic uint32_t len;                                    /**< Valid committed byte length of the written payload. */
} rbipc_slot_t;

/**
 * @struct rbipc_shm_header_t
 * @brief Control header residing at byte offset 0 of the POSIX shared memory file.
 * @details Encapsulates geometric dimensions, protocol versioning, and cache-line isolated
 * atomic synchronization counters for coordinating producers and consumers.
 */
typedef struct {
    uint64_t magic;              /**< Structural header identification signature (@ref RBIPC_MAGIC). */
    uint32_t version;            /**< Header protocol layout format version (@ref RBIPC_VERSION). */
    uint32_t header_size;        /**< Exact byte size of this control header structure. */
    uint64_t total_shm_size;     /**< Total file size in bytes of the shared memory region. */
    uint64_t data_offset;        /**< Page-aligned byte offset from base where payload ring data begins. */
    uint64_t data_size;          /**< Page-aligned byte size of the single circular buffer. */
    uint32_t capacity;           /**< Total number of slot descriptors (strictly a power of two). */
    uint32_t capacity_mask;      /**< Fast bitwise indexing mask, precomputed as (capacity - 1). */
    uint32_t slot_size;          /**< Maximum allowable payload byte capacity allocated per slot. */
    uint32_t spin_threshold;     /**< Maximum iterations before evaluating peer process liveness. */

    /* Cache-line isolated atomic synchronization variables */
    _Alignas(RBIPC_CACHE_LINE) _Atomic uint32_t write_ticket;     /**< Monotonic ticket counter claimed by reserving producers. */
    _Alignas(RBIPC_CACHE_LINE) _Atomic uint32_t read_ticket;      /**< Monotonic ticket counter claimed by acquiring consumers. */
    _Alignas(RBIPC_CACHE_LINE) _Atomic uint32_t futex_seq;        /**< Futex word sequence counter for consumer wakeups (data ready). */
    _Alignas(RBIPC_CACHE_LINE) _Atomic uint32_t futex_waiters;    /**< Count of consumer threads actively sleeping on @c futex_seq. */
    _Alignas(RBIPC_CACHE_LINE) _Atomic uint32_t write_futex_seq;  /**< Futex word sequence counter for producer wakeups (space ready). */
    _Alignas(RBIPC_CACHE_LINE) _Atomic uint32_t write_waiters;    /**< Count of producer threads actively sleeping on @c write_futex_seq. */
    _Alignas(RBIPC_CACHE_LINE) _Atomic uint32_t active_producers; /**< Number of currently attached producer handles. */
    _Alignas(RBIPC_CACHE_LINE) _Atomic uint32_t active_consumers; /**< Number of currently attached consumer handles. */
    _Alignas(RBIPC_CACHE_LINE) _Atomic uint32_t shutdown_flag;    /**< Atomic flag set to 1 when cooperative shutdown is requested. */
} rbipc_shm_header_t;

/**
 * @struct rbipc_stats_t
 * @brief Point-in-time telemetry snapshot of ring buffer state and operational counters.
 */
typedef struct {
    uint64_t total_shm_size;     /**< Total size in bytes of the underlying shared memory file. */
    uint64_t data_size;          /**< Byte size of the single circular payload buffer. */
    uint32_t capacity;           /**< Total slot capacity of the ring buffer. */
    uint32_t slot_size;          /**< Maximum payload capacity in bytes per slot. */
    uint32_t write_ticket;       /**< Next ticket number to be claimed by writing producers. */
    uint32_t read_ticket;        /**< Next ticket number to be claimed by acquiring consumers. */
    uint32_t active_producers;   /**< Total count of attached active producer processes. */
    uint32_t active_consumers;   /**< Total count of attached active consumer processes. */
    uint32_t futex_waiters;      /**< Total count of consumer threads currently suspended in futex sleep. */
    bool is_shutdown;            /**< Boolean indicator confirming if ring shutdown has been signaled. */
} rbipc_stats_t;

/**
 * @struct rbipc_iovec_t
 * @brief Vector descriptor for B-Queue batch write reservations.
 */
typedef struct {
    void *buf;           /**< Direct pointer to reserved payload buffer in double-mapped virtual memory. */
    uint32_t ticket;     /**< Monotonic ticket identifier assigned to this slot reservation. */
    uint32_t max_len;    /**< Maximum allowable payload length in bytes for this slot. */
} rbipc_iovec_t;

/**
 * @struct rbipc_rovec_t
 * @brief Vector descriptor for B-Queue batch read acquisitions.
 */
typedef struct {
    const void *buf;     /**< Direct read-only pointer to payload data in double-mapped virtual memory. */
    uint32_t ticket;     /**< Monotonic ticket identifier assigned to this acquired slot. */
    uint32_t len;        /**< Exact byte length of the committed message payload. */
} rbipc_rovec_t;

/* ============================================================================
 * Opaque Local Handle
 * ============================================================================ */

/**
 * @typedef rbipc_ring_t
 * @brief Opaque process-local handle referencing an attached ring buffer instance.
 */
typedef struct rbipc_ring rbipc_ring_t;

/* ============================================================================
 * Public API Surface
 * ============================================================================ */

/**
 * @brief Create and initialize a new shared-memory IPC ring buffer.
 *
 * @details Allocates a POSIX shared memory object or an anonymous memfd, truncates it to
 * the precomputed geometric size (control header, cache-aligned slot table, and page-aligned
 * payload buffer), configures Linux file seals (F_SEAL_SHRINK | F_SEAL_GROW), initializes
 * slot sequence state machines, and establishes the double-mapped virtual address region.
 *
 * @param[in]  name      POSIX shared memory object path (e.g. "/my_ring"). If NULL, an anonymous
 *                       memfd is created that can be passed across UNIX sockets via SCM_RIGHTS.
 * @param[in]  capacity  Requested slot count. Automatically rounded up to the nearest power of two.
 *                       Must be between 2 and 2^30.
 * @param[in]  slot_size Maximum allowable payload byte capacity per slot.
 * @param[out] out_ring  Location to store the pointer to the newly allocated ring handle.
 *
 * @return @ref RBIPC_OK on successful initialization.
 * @retval RBIPC_ERR_INVAL if parameters are invalid (null @p out_ring, zero @p slot_size, invalid @p name).
 * @retval RBIPC_ERR_NOMEM if process virtual memory allocation or mmap fails.
 * @retval RBIPC_ERR_SYS   if an underlying kernel syscall fails (shm_open, ftruncate, fcntl).
 *
 * @see rbipc_attach
 * @see rbipc_detach
 * @see rbipc_destroy
 */
RBIPC_NODISCARD RBIPC_LEAF
int rbipc_create(const char * RBIPC_RESTRICT name, size_t capacity, uint32_t slot_size, rbipc_ring_t ** RBIPC_RESTRICT out_ring);

/**
 * @brief Attach to an existing named shared-memory IPC ring buffer.
 *
 * @details Opens an existing POSIX shared memory object by name, probes and verifies the
 * structural integrity of the control header (@ref RBIPC_MAGIC, version, power-of-two capacity),
 * maps the metadata and double-mapped data regions into the caller's virtual address space,
 * and increments the active attachee counter.
 *
 * @param[in]  name     POSIX shared memory object path (e.g. "/my_ring").
 * @param[out] out_ring Location to store the pointer to the allocated ring handle.
 *
 * @return @ref RBIPC_OK on success.
 * @retval RBIPC_ERR_INVAL if @p name or @p out_ring is NULL or invalid.
 * @retval RBIPC_ERR_NOMEM if virtual address space mapping fails.
 * @retval RBIPC_ERR_SYS   if shm_open or fstat fails (e.g. ENOENT if ring does not exist).
 *
 * @see rbipc_create
 * @see rbipc_attach_fd
 * @see rbipc_detach
 */
RBIPC_NODISCARD RBIPC_LEAF
int rbipc_attach(const char * RBIPC_RESTRICT name, rbipc_ring_t ** RBIPC_RESTRICT out_ring);

/**
 * @brief Attach to an existing shared-memory IPC ring buffer via open file descriptor.
 *
 * @details Facilitates anonymous IPC across process boundaries where file descriptors
 * are inherited via fork() or received across UNIX domain sockets via SCM_RIGHTS ancillary data.
 * Validates header geometry and establishes double-mapped virtual addresses.
 *
 * @param[in]  fd       Open, readable and writable file descriptor referencing the shared memory object.
 * @param[out] out_ring Location to store the pointer to the allocated ring handle.
 *
 * @return @ref RBIPC_OK on success.
 * @retval RBIPC_ERR_INVAL if @p fd is invalid or @p out_ring is NULL.
 * @retval RBIPC_ERR_NOMEM if memory mapping fails.
 * @retval RBIPC_ERR_SYS   if descriptor validation or memory mapping fails.
 *
 * @see rbipc_attach
 * @see rbipc_get_fd
 */
RBIPC_NODISCARD RBIPC_LEAF
int rbipc_attach_fd(int fd, rbipc_ring_t ** RBIPC_RESTRICT out_ring);

/**
 * @brief Detach from the ring buffer, unmapping virtual memory regions and closing handles.
 *
 * @details Decrements active attachee counters in the control header, unmaps metadata and
 * double-mapped circular memory windows via munmap, closes open file descriptors, and frees
 * process-local handle memory. Does not unlink named shared memory objects from the system.
 *
 * @param[in,out] ring Pointer to ring buffer handle allocated by create or attach.
 *
 * @return @ref RBIPC_OK on successful detachment.
 * @retval RBIPC_ERR_INVAL if @p ring is NULL.
 *
 * @see rbipc_destroy
 */
RBIPC_LEAF
int rbipc_detach(rbipc_ring_t *ring);

/**
 * @brief Unlink and remove the named shared memory object from the operating system.
 *
 * @details Calls shm_unlink() to dissociate the object name from the filesystem namespace.
 * Existing attached processes retain access until all handles are detached.
 *
 * @param[in] name POSIX shared memory name (e.g. "/my_ring").
 *
 * @return @ref RBIPC_OK on success.
 * @retval RBIPC_ERR_INVAL if @p name is NULL.
 * @retval RBIPC_ERR_SYS   if shm_unlink fails (e.g. ENOENT).
 *
 * @see rbipc_create
 * @see rbipc_detach
 */
RBIPC_LEAF
int rbipc_destroy(const char *name);

/**
 * @brief Reserve a slot in the ring buffer for writing (Zero-Copy, Blocking).
 *
 * @details Atomically reserves the next available sequential slot using ticket-based CAS.
 * If the ring buffer is completely full, suspends the thread immediately via Linux sys_futex
 * in a zero-spin passive wait until consumers release occupied slots. On success, provides
 * direct pointer access to the double-mapped slot buffer for in-place writing.
 *
 * @param[in]  ring       Ring buffer handle.
 * @param[in]  len        Requested payload length in bytes (must satisfy 0 < len <= slot_size).
 * @param[out] out_buf    Direct memory pointer into the double-mapped payload buffer.
 * @param[out] ticket     Assigned ticket number, required for subsequent commit or abort.
 *
 * @return @ref RBIPC_OK on successful slot reservation.
 * @retval RBIPC_ERR_INVAL    if arguments are null or @p len exceeds configured slot capacity.
 * @retval RBIPC_ERR_SHUTDOWN if ring buffer has entered shutdown state.
 *
 * @note The returned @p out_buf is guaranteed to be cache-line aligned (64 bytes).
 * @warning The caller MUST call @ref rbipc_commit_write or @ref rbipc_abort_write with @p ticket.
 *
 * @see rbipc_commit_write
 * @see rbipc_abort_write
 * @see rbipc_reserve_write_timeout
 * @see rbipc_reserve_write_nonblock
 */
RBIPC_NODISCARD RBIPC_HOT RBIPC_LEAF
int rbipc_reserve_write(rbipc_ring_t * RBIPC_RESTRICT ring, uint32_t len,
                        void ** RBIPC_RESTRICT out_buf, uint32_t * RBIPC_RESTRICT ticket);

/**
 * @brief Reserve a slot in the ring buffer for writing with timeout (Zero-Copy).
 *
 * @details Identical to @ref rbipc_reserve_write, with a bounded timeout in nanoseconds.
 *
 * @param[in]  ring       Ring buffer handle.
 * @param[in]  len        Requested payload length in bytes.
 * @param[in]  timeout_ns Timeout duration in nanoseconds (0 for non-blocking attempt).
 * @param[out] out_buf    Direct memory pointer into the double-mapped payload buffer.
 * @param[out] ticket     Assigned ticket number.
 *
 * @return @ref RBIPC_OK on success.
 * @retval RBIPC_ERR_TIMEOUT  if deadline expires before space becomes available.
 * @retval RBIPC_ERR_FULL     if @p timeout_ns is 0 and no slot is immediately vacant.
 * @retval RBIPC_ERR_SHUTDOWN if ring buffer is shutting down.
 * @retval RBIPC_ERR_INVAL    if arguments are invalid.
 *
 * @see rbipc_reserve_write
 */
RBIPC_NODISCARD RBIPC_LEAF
int rbipc_reserve_write_timeout(rbipc_ring_t * RBIPC_RESTRICT ring, uint32_t len, uint64_t timeout_ns,
                                void ** RBIPC_RESTRICT out_buf, uint32_t * RBIPC_RESTRICT ticket);

/**
 * @brief Non-blocking write reservation attempt (Zero-Copy).
 *
 * @details Attempts immediate single-pass reservation of a slot. Never enters futex sleep.
 *
 * @param[in]  ring       Ring buffer handle.
 * @param[in]  len        Requested payload length in bytes.
 * @param[out] out_buf    Direct memory pointer into the double-mapped payload buffer.
 * @param[out] ticket     Assigned ticket number.
 *
 * @return @ref RBIPC_OK on success.
 * @retval RBIPC_ERR_FULL     if the ring buffer is currently full.
 * @retval RBIPC_ERR_SHUTDOWN if ring buffer is shutting down.
 * @retval RBIPC_ERR_INVAL    if arguments are invalid.
 *
 * @see rbipc_reserve_write
 */
RBIPC_NODISCARD RBIPC_HOT RBIPC_LEAF
int rbipc_reserve_write_nonblock(rbipc_ring_t * RBIPC_RESTRICT ring, uint32_t len,
                                 void ** RBIPC_RESTRICT out_buf, uint32_t * RBIPC_RESTRICT ticket);

/**
 * @brief Commit a previously reserved slot, publishing its payload to consumers.
 *
 * @details Stores the written byte length, transitions the slot state to COMMITTED, and
 * advances the slot sequence with release memory ordering. Evaluates waiter counters and
 * triggers a sys_futex wake only if sleeping consumers are present.
 *
 * @param[in,out] ring        Ring buffer handle.
 * @param[in]     ticket      Ticket number obtained from @ref rbipc_reserve_write.
 * @param[in]     written_len Actual byte length written into the payload buffer.
 *
 * @return @ref RBIPC_OK on successful commit.
 * @retval RBIPC_ERR_INVAL if @p ring is NULL or @p written_len exceeds slot size.
 *
 * @see rbipc_reserve_write
 * @see rbipc_abort_write
 */
RBIPC_HOT RBIPC_LEAF
int rbipc_commit_write(rbipc_ring_t * RBIPC_RESTRICT ring, uint32_t ticket, uint32_t written_len);

/**
 * @brief Abort a previously reserved slot without publishing payload data.
 *
 * @details Marks the slot state as POISONED and increments its sequence turn. Consumers
 * encountering this slot will safely skip over it without deadlock or stalling.
 *
 * @param[in,out] ring   Ring buffer handle.
 * @param[in]     ticket Ticket number obtained from @ref rbipc_reserve_write.
 *
 * @return @ref RBIPC_OK on successful abort.
 * @retval RBIPC_ERR_INVAL if @p ring is NULL.
 *
 * @see rbipc_reserve_write
 * @see rbipc_commit_write
 */
RBIPC_COLD RBIPC_LEAF
int rbipc_abort_write(rbipc_ring_t * RBIPC_RESTRICT ring, uint32_t ticket);

/**
 * @brief Acquire the next committed message for reading (Zero-Copy, Blocking).
 *
 * @details Claims the next read ticket in sequence. If the corresponding slot is not yet
 * committed, enters passive futex suspension until the producer publishes the message.
 * Audits the producer PID; if the producer crashes before committing, marks the slot POISONED
 * and returns @ref RBIPC_ERR_POISONED to allow the pipeline to proceed seamlessly.
 *
 * @param[in,out] ring       Ring buffer handle.
 * @param[out]    out_buf    Direct read-only pointer to payload in double-mapped memory.
 * @param[out]    out_len    Pointer receiving committed payload byte length.
 * @param[out]    ticket     Assigned ticket number, required for subsequent release.
 *
 * @return @ref RBIPC_OK on successful message acquisition.
 * @retval RBIPC_ERR_POISONED if producer crashed; slot skipped safely.
 * @retval RBIPC_ERR_SHUTDOWN if ring buffer has been shut down and drained.
 * @retval RBIPC_ERR_INVAL    if arguments are null.
 *
 * @note Payload memory remains valid until @ref rbipc_read_release is called.
 * @see rbipc_read_release
 * @see rbipc_read_acquire_timeout
 * @see rbipc_read_acquire_nonblock
 */
RBIPC_NODISCARD RBIPC_HOT RBIPC_LEAF
int rbipc_read_acquire(rbipc_ring_t * RBIPC_RESTRICT ring, const void ** RBIPC_RESTRICT out_buf,
                       uint32_t * RBIPC_RESTRICT out_len, uint32_t * RBIPC_RESTRICT ticket);

/**
 * @brief Acquire the next committed message with timeout (Zero-Copy).
 *
 * @details Identical to @ref rbipc_read_acquire, bounded by a nanosecond timeout.
 *
 * @param[in,out] ring       Ring buffer handle.
 * @param[in]     timeout_ns Timeout duration in nanoseconds (0 for non-blocking attempt).
 * @param[out]    out_buf    Direct read-only pointer to payload.
 * @param[out]    out_len    Pointer receiving payload length.
 * @param[out]    ticket     Assigned ticket number.
 *
 * @return @ref RBIPC_OK on success.
 * @retval RBIPC_ERR_TIMEOUT  if deadline expires before message is committed.
 * @retval RBIPC_ERR_EMPTY    if @p timeout_ns is 0 and no message is ready.
 * @retval RBIPC_ERR_POISONED if producer crashed.
 * @retval RBIPC_ERR_SHUTDOWN if ring buffer has shut down.
 * @retval RBIPC_ERR_INVAL    if arguments are invalid.
 *
 * @see rbipc_read_acquire
 */
RBIPC_NODISCARD RBIPC_LEAF
int rbipc_read_acquire_timeout(rbipc_ring_t * RBIPC_RESTRICT ring, uint64_t timeout_ns,
                               const void ** RBIPC_RESTRICT out_buf, uint32_t * RBIPC_RESTRICT out_len,
                               uint32_t * RBIPC_RESTRICT ticket);

/**
 * @brief Non-blocking read acquire attempt (Zero-Copy).
 *
 * @details Attempts immediate single-pass acquisition of a committed message without blocking.
 *
 * @param[in,out] ring       Ring buffer handle.
 * @param[out]    out_buf    Direct read-only pointer to payload.
 * @param[out]    out_len    Pointer receiving payload length.
 * @param[out]    ticket     Assigned ticket number.
 *
 * @return @ref RBIPC_OK on success.
 * @retval RBIPC_ERR_EMPTY    if no message is ready.
 * @retval RBIPC_ERR_POISONED if producer crashed.
 * @retval RBIPC_ERR_SHUTDOWN if ring buffer is shut down and drained.
 * @retval RBIPC_ERR_INVAL    if arguments are invalid.
 *
 * @see rbipc_read_acquire
 */
RBIPC_NODISCARD RBIPC_HOT RBIPC_LEAF
int rbipc_read_acquire_nonblock(rbipc_ring_t * RBIPC_RESTRICT ring, const void ** RBIPC_RESTRICT out_buf,
                                uint32_t * RBIPC_RESTRICT out_len, uint32_t * RBIPC_RESTRICT ticket);

/**
 * @brief Release a consumed slot back to the ring buffer for reuse.
 *
 * @details Transitions slot state back to EMPTY and advances its turn sequence by capacity.
 * If producer threads are sleeping waiting for space, dispatches a futex wakeup.
 *
 * @param[in,out] ring   Ring buffer handle.
 * @param[in]     ticket Ticket number obtained from @ref rbipc_read_acquire.
 *
 * @return @ref RBIPC_OK on successful slot release.
 * @retval RBIPC_ERR_INVAL if @p ring is NULL.
 *
 * @see rbipc_read_acquire
 */
RBIPC_HOT RBIPC_LEAF
int rbipc_read_release(rbipc_ring_t * RBIPC_RESTRICT ring, uint32_t ticket);

/**
 * @brief Reserve a batch of contiguous slots for writing (Zero-Copy, B-Queue Batching).
 *
 * @details Implements B-Queue vector batching to amortize atomic ticket claims and CAS
 * synchronization overhead over @p count slots in a single atomic fetch-and-add operation.
 *
 * @param[in,out] ring         Ring buffer handle.
 * @param[in]     count        Requested slot count (must be > 0 and <= capacity).
 * @param[out]    iovecs       Caller-provided array of at least @p count vector descriptors.
 * @param[out]    out_reserved Pointer receiving actual number of successfully reserved slots.
 *
 * @return @ref RBIPC_OK on success.
 * @retval RBIPC_ERR_INVAL    if arguments are null or count is zero.
 * @retval RBIPC_ERR_SHUTDOWN if ring buffer has shut down.
 *
 * @see rbipc_commit_write_batch
 * @see rbipc_read_acquire_batch
 */
RBIPC_NODISCARD RBIPC_HOT RBIPC_LEAF
int rbipc_reserve_write_batch(rbipc_ring_t * RBIPC_RESTRICT ring, uint32_t count,
                              rbipc_iovec_t * RBIPC_RESTRICT iovecs, uint32_t * RBIPC_RESTRICT out_reserved);

/**
 * @brief Commit a batch of previously reserved slots.
 *
 * @details Publishes multiple reserved slots to consumers in ticket order, triggering a
 * single coalesced futex wake notification if consumers are sleeping.
 *
 * @param[in,out] ring    Ring buffer handle.
 * @param[in]     count   Number of slots to commit.
 * @param[in]     tickets Array of ticket numbers from @ref rbipc_reserve_write_batch.
 * @param[in]     lens    Array of written payload lengths for each slot.
 *
 * @return @ref RBIPC_OK on success.
 * @retval RBIPC_ERR_INVAL if arguments are null or any length exceeds slot capacity.
 *
 * @see rbipc_reserve_write_batch
 */
RBIPC_HOT RBIPC_LEAF
int rbipc_commit_write_batch(rbipc_ring_t * RBIPC_RESTRICT ring, uint32_t count,
                             const uint32_t * RBIPC_RESTRICT tickets, const uint32_t * RBIPC_RESTRICT lens);

/**
 * @brief Acquire a batch of committed messages for reading (Zero-Copy, B-Queue Batching).
 *
 * @details Claims up to @p count contiguous committed slots in a single atomic claim.
 *
 * @param[in,out] ring         Ring buffer handle.
 * @param[in]     count        Maximum number of messages to acquire.
 * @param[out]    rovecs       Caller-provided array of at least @p count vector descriptors.
 * @param[out]    out_acquired Pointer receiving number of messages successfully acquired.
 *
 * @return @ref RBIPC_OK on success.
 * @retval RBIPC_ERR_INVAL    if arguments are null or count is zero.
 * @retval RBIPC_ERR_SHUTDOWN if ring buffer has shut down and drained.
 *
 * @see rbipc_read_release_batch
 */
RBIPC_NODISCARD RBIPC_HOT RBIPC_LEAF
int rbipc_read_acquire_batch(rbipc_ring_t * RBIPC_RESTRICT ring, uint32_t count,
                             rbipc_rovec_t * RBIPC_RESTRICT rovecs, uint32_t * RBIPC_RESTRICT out_acquired);

/**
 * @brief Release a batch of consumed slots back to the ring buffer.
 *
 * @details Releases an array of consumed tickets in a single vector sweep, notifying
 * waiting producers with a single coalesced wakeup.
 *
 * @param[in,out] ring    Ring buffer handle.
 * @param[in]     count   Number of slots to release.
 * @param[in]     tickets Array of ticket numbers from @ref rbipc_read_acquire_batch.
 *
 * @return @ref RBIPC_OK on success.
 * @retval RBIPC_ERR_INVAL if arguments are null or count is zero.
 *
 * @see rbipc_read_acquire_batch
 */
RBIPC_HOT RBIPC_LEAF
int rbipc_read_release_batch(rbipc_ring_t * RBIPC_RESTRICT ring, uint32_t count,
                             const uint32_t * RBIPC_RESTRICT tickets);

/**
 * @brief Signal cooperative shutdown to all waiting producers and consumers.
 *
 * @details Atomically sets the shutdown flag and executes futex wakeups across both
 * producer and consumer channels to awaken blocked workers.
 *
 * @param[in,out] ring Ring buffer handle.
 *
 * @return @ref RBIPC_OK on success.
 * @retval RBIPC_ERR_INVAL if @p ring is NULL.
 *
 * @see rbipc_detach
 */
RBIPC_LEAF
int rbipc_signal_shutdown(rbipc_ring_t *ring);

/**
 * @brief Retrieve underlying shared memory file descriptor.
 *
 * @param[in] ring Ring buffer handle.
 *
 * @return Non-negative file descriptor on success, or -1 on invalid argument.
 *
 * @see rbipc_attach_fd
 */
RBIPC_NODISCARD RBIPC_PURE RBIPC_LEAF
int rbipc_get_fd(const rbipc_ring_t *ring);

/**
 * @brief Collect a point-in-time snapshot of ring buffer telemetry metrics.
 *
 * @details Captures memory geometry, current ticket positions, active producer/consumer
 * counts, and futex waiter metrics into @p out_stats.
 *
 * @param[in]  ring      Ring buffer handle.
 * @param[out] out_stats Output statistics snapshot structure.
 *
 * @return @ref RBIPC_OK on success.
 * @retval RBIPC_ERR_INVAL if @p ring or @p out_stats is NULL.
 */
RBIPC_NODISCARD RBIPC_LEAF
int rbipc_get_stats(const rbipc_ring_t * RBIPC_RESTRICT ring, rbipc_stats_t * RBIPC_RESTRICT out_stats);

/**
 * @brief Convert an error code into a human-readable description string.
 *
 * @param[in] err Return code from any librbipc API function.
 *
 * @return Static constant string describing the status code.
 */
RBIPC_RETURNS_NONNULL RBIPC_CONST RBIPC_COLD RBIPC_LEAF
const char *rbipc_strerror(int err);

#ifdef __cplusplus
}
#endif

#endif /* RBIPC_H */
