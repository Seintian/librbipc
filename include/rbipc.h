/**
 * @file rbipc.h
 * @brief librbipc: High-Performance Lock-Free Shared-Memory Ring Buffer IPC
 *
 * Designed for ultra-low latency, zero-copy inter-process communication in modern C (C11/C23).
 * Utilizes the virtual memory double-mapping mirror trick for seamless wraparound,
 * cache-line padded atomics to eliminate false sharing, and a 3-tier hybrid
 * synchronization strategy with dead-peer crash recovery.
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
 * Constants & Error Codes
 * ============================================================================ */

#define RBIPC_OK               0  /**< Success */
#define RBIPC_ERR_INVAL      (-1) /**< Invalid argument or configuration */
#define RBIPC_ERR_NOMEM      (-2) /**< Memory allocation / mapping failure */
#define RBIPC_ERR_SYS        (-3) /**< OS / System call error (inspect errno) */
#define RBIPC_ERR_FULL       (-4) /**< Ring buffer is currently full */
#define RBIPC_ERR_EMPTY      (-5) /**< Ring buffer is currently empty */
#define RBIPC_ERR_POISONED   (-6) /**< Slot producer crashed; slot poisoned */
#define RBIPC_ERR_SHUTDOWN   (-7) /**< Ring buffer signaled shutdown */
#define RBIPC_ERR_TIMEOUT    (-8) /**< Operation timed out */
#define RBIPC_ERR_BUSY       (-9) /**< Resource is busy / contention */
#define RBIPC_ERR_OVERFLOW  (-10) /**< Calculation / buffer overflow */

#define RBIPC_MAGIC         0x5242495043323032ULL /**< "RBIPC202" magic identifier */
#define RBIPC_VERSION       1
#define RBIPC_CACHE_LINE    64

/* ============================================================================
 * State Machine Constants
 * ============================================================================ */

enum rbipc_slot_state {
    RBIPC_SLOT_EMPTY     = 0, /**< Slot is free and available for write reservation */
    RBIPC_SLOT_RESERVED  = 1, /**< Slot is reserved by a producer (write in progress) */
    RBIPC_SLOT_COMMITTED = 2, /**< Slot payload is committed and ready for consumption */
    RBIPC_SLOT_POISONED  = 3  /**< Producer crashed during write; slot poisoned/bypassed */
};

/* ============================================================================
 * Shared Memory Data Structures
 * ============================================================================ */

/**
 * @struct rbipc_slot_t
 * @brief Cache-line aligned slot control descriptor in shared memory.
 */
typedef struct {
    _Alignas(RBIPC_CACHE_LINE) _Atomic uint32_t sequence;    /**< Monotonic sequence for turn scheduling */
    _Atomic uint32_t state;                                  /**< Slot state (enum rbipc_slot_state) */
    _Atomic uint32_t producer_pid;                           /**< PID of reserving producer (for crash detection) */
    _Atomic uint32_t len;                                    /**< Valid committed payload byte length */
} rbipc_slot_t;

/**
 * @struct rbipc_shm_header_t
 * @brief Control header residing at byte offset 0 of the shared memory region.
 */
typedef struct {
    uint64_t magic;              /**< Magic verification word */
    uint32_t version;            /**< Header version */
    uint32_t header_size;        /**< sizeof(rbipc_shm_header_t) */
    uint64_t total_shm_size;     /**< Total size of the shared memory file */
    uint64_t data_offset;        /**< Page-aligned offset to ring data buffer */
    uint64_t data_size;          /**< Byte size of single circular buffer (page-aligned) */
    uint32_t capacity;           /**< Number of slots (strictly a power of two) */
    uint32_t capacity_mask;      /**< capacity - 1 for fast bitwise indexing */
    uint32_t slot_size;          /**< Maximum payload size per slot */
    uint32_t spin_threshold;     /**< Spin-wait iterations before peer liveness check */

    /* Cache-line isolated hot atomic synchronization variables */
    _Alignas(RBIPC_CACHE_LINE) _Atomic uint32_t write_ticket;     /**< Monotonic ticket claimed by producers */
    _Alignas(RBIPC_CACHE_LINE) _Atomic uint32_t read_ticket;      /**< Monotonic ticket claimed by consumers */
    _Alignas(RBIPC_CACHE_LINE) _Atomic uint32_t futex_seq;        /**< Futex word for blocking synchronization */
    _Alignas(RBIPC_CACHE_LINE) _Atomic uint32_t futex_waiters;    /**< Count of threads waiting in futex sleep */
    _Alignas(RBIPC_CACHE_LINE) _Atomic uint32_t active_producers; /**< Count of attached producers */
    _Alignas(RBIPC_CACHE_LINE) _Atomic uint32_t active_consumers; /**< Count of attached consumers */
    _Alignas(RBIPC_CACHE_LINE) _Atomic uint32_t shutdown_flag;    /**< 1 if shutdown was signaled, else 0 */
} rbipc_shm_header_t;

/**
 * @struct rbipc_stats_t
 * @brief Snapshot of ring buffer statistics and operational state.
 */
typedef struct {
    uint64_t total_shm_size;     /**< Total size in bytes of the shared memory region */
    uint64_t data_size;          /**< Single circular buffer byte size */
    uint32_t capacity;           /**< Slot capacity */
    uint32_t slot_size;          /**< Byte capacity per slot */
    uint32_t write_ticket;       /**< Next ticket to be claimed by producers */
    uint32_t read_ticket;        /**< Next ticket to be claimed by consumers */
    uint32_t active_producers;   /**< Currently attached producers */
    uint32_t active_consumers;   /**< Currently attached consumers */
    uint32_t futex_waiters;      /**< Current threads suspended in futex sleep */
    bool is_shutdown;            /**< True if shutdown has been signaled */
} rbipc_stats_t;

/**
 * @struct rbipc_iovec_t
 * @brief Batch write reservation vector descriptor.
 */
typedef struct {
    void *buf;           /**< Direct pointer to payload buffer in double-mapped memory */
    uint32_t ticket;     /**< Monotonic ticket assigned to this slot */
    uint32_t max_len;    /**< Maximum allowable byte length for this slot */
} rbipc_iovec_t;

/**
 * @struct rbipc_rovec_t
 * @brief Batch read acquire vector descriptor.
 */
typedef struct {
    const void *buf;     /**< Direct pointer to payload in double-mapped memory */
    uint32_t ticket;     /**< Monotonic ticket assigned to this slot */
    uint32_t len;        /**< Actual payload byte length */
} rbipc_rovec_t;

/* ============================================================================
 * Opaque Local Handle
 * ============================================================================ */

typedef struct rbipc_ring rbipc_ring_t;

/* ============================================================================
 * Public API Surface
 * ============================================================================ */

/**
 * @brief Create and initialize a new shared-memory IPC ring buffer.
 *
 * Sets up POSIX shared memory, truncates to required layout, applies file seals,
 * and configures the double-mapped virtual address region for zero-copy wraparound.
 *
 * @param name Shared memory name (e.g. "/my_ring"). If NULL, anonymous memfd is created.
 * @param capacity Number of slots (will be rounded up to the next power of two).
 * @param slot_size Maximum payload byte size per slot.
 * @param[out] out_ring Returns pointer to allocated ring buffer handle.
 * @return RBIPC_OK on success, negative error code otherwise.
 */
int rbipc_create(const char *name, size_t capacity, uint32_t slot_size, rbipc_ring_t **out_ring);

/**
 * @brief Attach to an existing shared-memory IPC ring buffer by name.
 *
 * @param name Shared memory name (e.g. "/my_ring").
 * @param[out] out_ring Returns pointer to allocated ring buffer handle.
 * @return RBIPC_OK on success, negative error code otherwise.
 */
int rbipc_attach(const char *name, rbipc_ring_t **out_ring);

/**
 * @brief Attach to an existing shared-memory IPC ring buffer via open file descriptor.
 *
 * Useful when receiving an anonymous memfd via UNIX domain socket SCM_RIGHTS or inheritance.
 *
 * @param fd Open file descriptor to shared memory object.
 * @param[out] out_ring Returns pointer to allocated ring buffer handle.
 * @return RBIPC_OK on success, negative error code otherwise.
 */
int rbipc_attach_fd(int fd, rbipc_ring_t **out_ring);

/**
 * @brief Detach from the ring buffer, unmapping virtual memory and closing descriptors.
 *
 * @param ring Ring buffer handle.
 * @return RBIPC_OK on success, negative error code otherwise.
 */
int rbipc_detach(rbipc_ring_t *ring);

/**
 * @brief Unlink/destroy the shared memory object from the system.
 *
 * @param name Shared memory name (e.g. "/my_ring").
 * @return RBIPC_OK on success, negative error code otherwise.
 */
int rbipc_destroy(const char *name);

/**
 * @brief Reserve a slot in the ring buffer for writing (Zero-Copy, Blocking).
 *
 * Atomically reserves the next available slot according to ticket ordering.
 * If the ring is full, executes a 3-tier hybrid wait (spin -> yield -> futex).
 *
 * @param ring Ring buffer handle.
 * @param len Requested payload length (must be <= slot_size).
 * @param[out] out_buf Direct pointer into the double-mapped virtual memory buffer.
 * @param[out] ticket Returns assigned monotonically increasing ticket ID.
 * @return RBIPC_OK on success, RBIPC_ERR_SHUTDOWN, or negative error code.
 */
int rbipc_reserve_write(rbipc_ring_t *ring, uint32_t len, void **out_buf, uint32_t *ticket);

/**
 * @brief Reserve a slot in the ring buffer with timeout (Zero-Copy).
 *
 * @param ring Ring buffer handle.
 * @param len Requested payload length.
 * @param timeout_ns Timeout duration in nanoseconds (0 for non-blocking).
 * @param[out] out_buf Direct pointer into virtual buffer.
 * @param[out] ticket Returns assigned ticket ID.
 * @return RBIPC_OK on success, RBIPC_ERR_TIMEOUT, RBIPC_ERR_FULL, or negative error code.
 */
int rbipc_reserve_write_timeout(rbipc_ring_t *ring, uint32_t len, uint64_t timeout_ns,
                                void **out_buf, uint32_t *ticket);

/**
 * @brief Non-blocking write reservation attempt (Zero-Copy).
 *
 * @param ring Ring buffer handle.
 * @param len Requested payload length.
 * @param[out] out_buf Direct pointer into virtual buffer.
 * @param[out] ticket Returns assigned ticket ID.
 * @return RBIPC_OK on success, RBIPC_ERR_FULL if no space immediately available, or negative error code.
 */
int rbipc_reserve_write_nonblock(rbipc_ring_t *ring, uint32_t len, void **out_buf, uint32_t *ticket);

/**
 * @brief Commit a previously reserved slot, publishing it to consumers.
 *
 * @param ring Ring buffer handle.
 * @param ticket Ticket obtained from rbipc_reserve_write().
 * @param written_len Actual number of bytes written.
 * @return RBIPC_OK on success, negative error code otherwise.
 */
int rbipc_commit_write(rbipc_ring_t *ring, uint32_t ticket, uint32_t written_len);

/**
 * @brief Abort a previously reserved slot without publishing payload.
 *
 * Transitions slot to POISONED so consumers can safely bypass it without stalling.
 *
 * @param ring Ring buffer handle.
 * @param ticket Ticket obtained from rbipc_reserve_write().
 * @return RBIPC_OK on success, negative error code otherwise.
 */
int rbipc_abort_write(rbipc_ring_t *ring, uint32_t ticket);

/**
 * @brief Acquire the next committed message for reading (Zero-Copy, Blocking).
 *
 * Blocks using hybrid backoff until a message is committed.
 * If the holding producer crashed before committing, detects ESRCH, poisons
 * the slot, and returns RBIPC_ERR_POISONED while safely advancing the pipeline.
 *
 * @param ring Ring buffer handle.
 * @param[out] out_buf Direct pointer to payload in the double-mapped memory.
 * @param[out] out_len Returns byte length of published payload.
 * @param[out] ticket Returns ticket ID to be passed to rbipc_read_release().
 * @return RBIPC_OK on success, RBIPC_ERR_POISONED if skipped, RBIPC_ERR_SHUTDOWN, etc.
 */
int rbipc_read_acquire(rbipc_ring_t *ring, const void **out_buf, uint32_t *out_len, uint32_t *ticket);

/**
 * @brief Acquire the next committed message with timeout (Zero-Copy).
 *
 * @param ring Ring buffer handle.
 * @param timeout_ns Timeout duration in nanoseconds (0 for non-blocking).
 * @param[out] out_buf Direct pointer to payload.
 * @param[out] out_len Returns byte length of payload.
 * @param[out] ticket Returns ticket ID.
 * @return RBIPC_OK on success, RBIPC_ERR_TIMEOUT, RBIPC_ERR_EMPTY, or negative error code.
 */
int rbipc_read_acquire_timeout(rbipc_ring_t *ring, uint64_t timeout_ns,
                               const void **out_buf, uint32_t *out_len, uint32_t *ticket);

/**
 * @brief Non-blocking read acquire attempt (Zero-Copy).
 *
 * @param ring Ring buffer handle.
 * @param[out] out_buf Direct pointer to payload.
 * @param[out] out_len Returns byte length of payload.
 * @param[out] ticket Returns ticket ID.
 * @return RBIPC_OK on success, RBIPC_ERR_EMPTY if no committed message, or negative error code.
 */
int rbipc_read_acquire_nonblock(rbipc_ring_t *ring, const void **out_buf, uint32_t *out_len, uint32_t *ticket);

/**
 * @brief Release a consumed slot back to the ring buffer for reuse.
 *
 * @param ring Ring buffer handle.
 * @param ticket Ticket obtained from rbipc_read_acquire().
 * @return RBIPC_OK on success, negative error code otherwise.
 */
int rbipc_read_release(rbipc_ring_t *ring, uint32_t ticket);

/**
 * @brief Reserve a batch of contiguous slots for writing (Zero-Copy, B-Queue Batching).
 *
 * Atomically reserves up to `count` slots in a single atomic operation,
 * amortizing CAS synchronization overhead across multiple elements.
 *
 * @param ring Ring buffer handle.
 * @param count Desired number of slots to reserve (must be > 0 and <= capacity).
 * @param iovecs Array of at least `count` rbipc_iovec_t elements.
 * @param[out] out_reserved Returns number of slots reserved (at least 1 on success).
 * @return RBIPC_OK on success, RBIPC_ERR_FULL, RBIPC_ERR_SHUTDOWN, or negative error code.
 */
int rbipc_reserve_write_batch(rbipc_ring_t *ring, uint32_t count,
                              rbipc_iovec_t *iovecs, uint32_t *out_reserved);

/**
 * @brief Commit a batch of previously reserved slots.
 *
 * Publishes the slots to consumers in ticket order.
 *
 * @param ring Ring buffer handle.
 * @param count Number of slots to commit.
 * @param tickets Array of ticket numbers from rbipc_reserve_write_batch().
 * @param lens Array of written payload lengths for each slot.
 * @return RBIPC_OK on success, negative error code otherwise.
 */
int rbipc_commit_write_batch(rbipc_ring_t *ring, uint32_t count,
                             const uint32_t *tickets, const uint32_t *lens);

/**
 * @brief Acquire a batch of committed messages for reading (Zero-Copy, B-Queue Batching).
 *
 * Atomically claims up to `count` committed messages in a single atomic operation.
 *
 * @param ring Ring buffer handle.
 * @param count Maximum number of messages to acquire.
 * @param rovecs Array of at least `count` rbipc_rovec_t elements.
 * @param[out] out_acquired Returns number of messages acquired.
 * @return RBIPC_OK on success, RBIPC_ERR_EMPTY, RBIPC_ERR_SHUTDOWN, or negative error code.
 */
int rbipc_read_acquire_batch(rbipc_ring_t *ring, uint32_t count,
                             rbipc_rovec_t *rovecs, uint32_t *out_acquired);

/**
 * @brief Release a batch of consumed slots back to the ring buffer.
 *
 * @param ring Ring buffer handle.
 * @param count Number of slots to release.
 * @param tickets Array of tickets from rbipc_read_acquire_batch().
 * @return RBIPC_OK on success, negative error code otherwise.
 */
int rbipc_read_release_batch(rbipc_ring_t *ring, uint32_t count, const uint32_t *tickets);

/**
 * @brief Signal shutdown to all waiting producers and consumers.
 *
 * @param ring Ring buffer handle.
 * @return RBIPC_OK on success.
 */
int rbipc_signal_shutdown(rbipc_ring_t *ring);

/**
 * @brief Retrieve underlying shared memory file descriptor.
 *
 * @param ring Ring buffer handle.
 * @return File descriptor or -1 on error.
 */
int rbipc_get_fd(const rbipc_ring_t *ring);

/**
 * @brief Collect a point-in-time snapshot of ring buffer metrics.
 *
 * @param ring Ring buffer handle.
 * @param[out] out_stats Output statistics struct.
 * @return RBIPC_OK on success, negative error code otherwise.
 */
int rbipc_get_stats(const rbipc_ring_t *ring, rbipc_stats_t *out_stats);

/**
 * @brief Convert an error code into a human-readable description string.
 *
 * @param err Return code from any rbipc function.
 * @return Static string describing the error.
 */
const char *rbipc_strerror(int err);

#ifdef __cplusplus
}
#endif

#endif /* RBIPC_H */
