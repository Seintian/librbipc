/**
 * @file rbipc_pubsub.h
 * @brief 1-to-N Publish-Subscribe Multicast Broadcast Engine for librbipc.
 *
 * @details Implements lock-free multicast distribution where one or more
 * publishers broadcast messages to N concurrent subscribers. Each subscriber
 * maintains an independent reading cursor, eliminating consumer contention and
 * head-of-line blocking while enabling real-time telemetry, market data, and
 * sensor bus fanout.
 *
 * @author Seintian
 * @date 2026
 * @copyright Apache License 2.0
 */

#ifndef RBIPC_PUBSUB_H
#define RBIPC_PUBSUB_H

#include "rbipc.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @struct rbipc_sub_t
 * @brief Opaque subscriber handle tracking an independent topic reading cursor.
 */
typedef struct rbipc_sub rbipc_sub_t;

/**
 * @brief Publish a message to all subscribers on the ring buffer topic.
 *
 * @param[in] ring Ring buffer instance acting as the multicast topic.
 * @param[in] buf  Pointer to payload data to publish.
 * @param[in] len  Length of payload data in bytes (must be <= ring slot_size).
 *
 * @return @ref RBIPC_OK on success.
 * @retval RBIPC_ERR_INVAL if parameters are NULL or len exceeds slot capacity.
 */
RBIPC_API RBIPC_NODISCARD RBIPC_LEAF int rbipc_publish(rbipc_ring_t *ring,
                                             const void *buf, uint32_t len);

/**
 * @brief Reserve a slot in shared memory for zero-copy multicast publication.
 *
 * @details Claims the next write ticket in the topic ring buffer, reserves the slot,
 * and yields a direct memory pointer into shared memory so the caller can serialize
 * or write payload data directly into the slot without intermediate buffer copies.
 * The publication must be completed via @ref rbipc_publish_commit or cancelled
 * via @ref rbipc_publish_abort.
 *
 * @param[in]  ring       Ring buffer instance acting as the multicast topic.
 * @param[in]  len        Anticipated maximum payload byte length (must be <= ring slot_size).
 * @param[out] out_buf    Pointer to receive the direct shared-memory payload buffer.
 * @param[out] out_ticket Pointer to receive the assigned publication ticket.
 *
 * @return @ref RBIPC_OK on success.
 * @retval RBIPC_ERR_INVAL if parameters are NULL or len exceeds slot capacity.
 */
RBIPC_API RBIPC_NODISCARD RBIPC_LEAF int
rbipc_publish_reserve(rbipc_ring_t *ring, uint32_t len, void **out_buf,
                      uint64_t *out_ticket);

/**
 * @brief Commit a previously reserved zero-copy multicast publication.
 *
 * @details Publishes the payload written into the reserved slot, advancing the sequence
 * turn and waking suspended topic subscribers with memory release ordering.
 *
 * @param[in] ring   Ring buffer instance acting as the multicast topic.
 * @param[in] ticket Reservation ticket returned by @ref rbipc_publish_reserve.
 * @param[in] len    Actual payload byte length written (must be <= ring slot_size).
 *
 * @return @ref RBIPC_OK on success.
 * @retval RBIPC_ERR_INVAL if parameters are NULL or len exceeds slot capacity.
 */
RBIPC_API RBIPC_NODISCARD RBIPC_LEAF int
rbipc_publish_commit(rbipc_ring_t *ring, uint64_t ticket, uint32_t len);

/**
 * @brief Abort a previously reserved multicast publication.
 *
 * @details Transitions the reserved slot to @ref RBIPC_SLOT_POISONED, allowing
 * subscribers to safely bypass the abandoned ticket without blocking or deadlocking.
 *
 * @param[in] ring   Ring buffer instance acting as the multicast topic.
 * @param[in] ticket Reservation ticket returned by @ref rbipc_publish_reserve.
 *
 * @return @ref RBIPC_OK on success.
 * @retval RBIPC_ERR_INVAL if @p ring is NULL.
 */
RBIPC_API RBIPC_NODISCARD RBIPC_LEAF int
rbipc_publish_abort(rbipc_ring_t *ring, uint64_t ticket);

/**
 * @brief Publish a contiguous batch of messages to all subscribers on the ring
 * buffer topic.
 *
 * @details Amortizes the atomic write ticket increment and sequential
 * consistency fences across
 * @p count messages, applying cache-bypassing non-temporal streaming stores for
 * payloads >= 256 bytes.
 *
 * @param[in] ring  Ring buffer instance acting as the multicast topic.
 * @param[in] count Number of messages in batch arrays.
 * @param[in] bufs  Array of pointers to payload data buffers.
 * @param[in] lens  Array of payload byte lengths (each must be <= ring
 * slot_size).
 *
 * @return @ref RBIPC_OK on success.
 * @retval RBIPC_ERR_INVAL if parameters are NULL, count is 0, or any length
 * exceeds slot capacity.
 */
RBIPC_API RBIPC_NODISCARD RBIPC_LEAF int rbipc_publish_batch(rbipc_ring_t *ring,
                                                   uint32_t count,
                                                   const void *const *bufs,
                                                   const uint32_t *lens);

/**
 * @brief Create a local subscriber handle attached to a topic ring buffer.
 *
 * @param[in]  ring         Ring buffer instance.
 * @param[in]  from_oldest  If true, start reading from oldest available
 * buffered message; if false, start reading from current published head.
 * @param[out] out_sub      Pointer to receive the allocated subscriber handle.
 *
 * @return @ref RBIPC_OK on success.
 * @retval RBIPC_ERR_INVAL if @p ring or @p out_sub is NULL.
 * @retval RBIPC_ERR_NOMEM if allocation fails.
 */
RBIPC_API RBIPC_NODISCARD RBIPC_LEAF int rbipc_sub_create_ex(rbipc_ring_t *ring,
                                                   bool from_oldest,
                                                   rbipc_sub_t **out_sub);

/**
 * @brief Create a subscriber handle starting from the oldest available message.
 *
 * @param[in]  ring    Ring buffer instance.
 * @param[out] out_sub Pointer to receive the allocated subscriber handle.
 *
 * @return @ref RBIPC_OK on success.
 * @retval RBIPC_ERR_INVAL if @p ring or @p out_sub is NULL.
 * @retval RBIPC_ERR_NOMEM if allocation fails.
 */
RBIPC_API RBIPC_NODISCARD RBIPC_LEAF int rbipc_sub_create(rbipc_ring_t *ring,
                                                rbipc_sub_t **out_sub);

/**
 * @brief Destroy a subscriber handle and release process-local resources.
 *
 * @param[in] sub Subscriber handle to destroy.
 */
RBIPC_API RBIPC_LEAF
void rbipc_sub_destroy(rbipc_sub_t *sub);

/**
 * @brief Read the next published message non-blockingly.
 *
 * @param[in]  sub      Subscriber handle.
 * @param[out] out_buf  Pointer to receive payload address.
 * @param[out] out_len  Pointer to receive payload length in bytes.
 * @param[out] out_seq  Pointer to receive message sequence number.
 *
 * @return @ref RBIPC_OK on success.
 * @retval RBIPC_ERR_INVAL if parameters are NULL.
 * @retval RBIPC_ERR_EMPTY if no new published messages are available.
 * @retval RBIPC_ERR_POISONED if the encountered slot was aborted/poisoned.
 * @retval RBIPC_ERR_OVERFLOW if subscriber lag exceeded topic capacity (cursor was clamped to oldest valid slot, notifying caller of dropped messages).
 */
RBIPC_API RBIPC_NODISCARD RBIPC_LEAF int rbipc_sub_poll(rbipc_sub_t *sub,
                                              const void **out_buf,
                                              uint32_t *out_len,
                                              uint64_t *out_seq);

/**
 * @brief Read a batch of next published messages non-blockingly.
 *
 * @details Vectorized subscriber reading that extracts up to @p max_count
 * published messages without per-slot synchronization overhead, advancing the
 * local subscriber cursor.
 *
 * @param[in]  sub        Subscriber handle.
 * @param[in]  max_count  Maximum number of messages to poll into output arrays.
 * @param[out] out_bufs   Array of pointers to receive payload addresses in
 * shared memory.
 * @param[out] out_lens   Array to receive payload byte lengths.
 * @param[out] out_seqs   Array to receive message sequence numbers.
 * @param[out] out_polled Pointer to receive actual count of polled messages (0
 * to max_count).
 *
 * @return @ref RBIPC_OK on success.
 * @retval RBIPC_ERR_INVAL if parameters are NULL or max_count is 0.
 * @retval RBIPC_ERR_POISONED if the first encountered slot was aborted/poisoned.
 * @retval RBIPC_ERR_OVERFLOW if subscriber lag exceeded topic capacity (cursor was clamped to oldest valid slot, notifying caller of dropped messages).
 */
RBIPC_API RBIPC_NODISCARD RBIPC_LEAF int
rbipc_sub_poll_batch(rbipc_sub_t *sub, uint32_t max_count,
                     const void **out_bufs, uint32_t *out_lens,
                     uint64_t *out_seqs, uint32_t *out_polled);

/**
 * @brief Wait for and read the next published message with timeout.
 *
 * @param[in]  sub        Subscriber handle.
 * @param[in]  timeout_ns Timeout budget in nanoseconds (0 for non-blocking).
 * @param[out] out_buf    Pointer to receive payload address.
 * @param[out] out_len    Pointer to receive payload length in bytes.
 * @param[out] out_seq    Pointer to receive message sequence number.
 *
 * @return @ref RBIPC_OK on success.
 * @retval RBIPC_ERR_INVAL if parameters are NULL.
 * @retval RBIPC_ERR_TIMEOUT if timeout budget expired before a message was
 * published.
 * @retval RBIPC_ERR_SHUTDOWN if the topic ring was shut down.
 * @retval RBIPC_ERR_POISONED if the encountered slot was aborted/poisoned.
 * @retval RBIPC_ERR_OVERFLOW if subscriber lag exceeded topic capacity (cursor was clamped to oldest valid slot, notifying caller of dropped messages).
 */
RBIPC_API RBIPC_NODISCARD RBIPC_LEAF int
rbipc_sub_read(rbipc_sub_t *sub, uint64_t timeout_ns, const void **out_buf,
               uint32_t *out_len, uint64_t *out_seq);

/**
 * @brief Retrieve number of unread messages currently pending for this
 * subscriber.
 *
 * @param[in] sub Subscriber handle.
 *
 * @return Pending message count (lag distance).
 */
RBIPC_API RBIPC_NODISCARD RBIPC_LEAF uint64_t rbipc_sub_get_lag(const rbipc_sub_t *sub);

#ifdef __cplusplus
}
#endif

#endif /* RBIPC_PUBSUB_H */
