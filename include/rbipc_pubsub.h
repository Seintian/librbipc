/**
 * @file rbipc_pubsub.h
 * @brief 1-to-N Publish-Subscribe Multicast Broadcast Engine for librbipc.
 *
 * @details Implements lock-free multicast distribution where one or more publishers broadcast
 * messages to N concurrent subscribers. Each subscriber maintains an independent reading cursor,
 * eliminating consumer contention and head-of-line blocking while enabling real-time telemetry,
 * market data, and sensor bus fanout.
 *
 * @author Christian Santarelli
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
RBIPC_NODISCARD RBIPC_LEAF
int rbipc_publish(rbipc_ring_t *ring, const void *buf, uint32_t len);

/**
 * @brief Create a local subscriber handle attached to a topic ring buffer.
 *
 * @param[in]  ring         Ring buffer instance.
 * @param[in]  from_oldest  If true, start reading from oldest available buffered message;
 *                          if false, start reading from current published head.
 * @param[out] out_sub      Pointer to receive the allocated subscriber handle.
 *
 * @return @ref RBIPC_OK on success.
 * @retval RBIPC_ERR_INVAL if @p ring or @p out_sub is NULL.
 * @retval RBIPC_ERR_NOMEM if allocation fails.
 */
RBIPC_NODISCARD RBIPC_LEAF
int rbipc_sub_create_ex(rbipc_ring_t *ring, bool from_oldest, rbipc_sub_t **out_sub);

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
RBIPC_NODISCARD RBIPC_LEAF
int rbipc_sub_create(rbipc_ring_t *ring, rbipc_sub_t **out_sub);

/**
 * @brief Destroy a subscriber handle and release process-local resources.
 *
 * @param[in] sub Subscriber handle to destroy.
 */
RBIPC_LEAF
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
 */
RBIPC_NODISCARD RBIPC_LEAF
int rbipc_sub_poll(rbipc_sub_t *sub, const void **out_buf, uint32_t *out_len, uint64_t *out_seq);

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
 * @retval @ref RBIPC_ERR_TIMEOUT if timeout budget expired before a message was published.
 * @retval @ref RBIPC_ERR_SHUTDOWN if the topic ring was shut down.
 */
RBIPC_NODISCARD RBIPC_LEAF
int rbipc_sub_read(rbipc_sub_t *sub, uint64_t timeout_ns, const void **out_buf, uint32_t *out_len, uint64_t *out_seq);

/**
 * @brief Retrieve number of unread messages currently pending for this subscriber.
 *
 * @param[in] sub Subscriber handle.
 *
 * @return Pending message count (lag distance).
 */
RBIPC_NODISCARD RBIPC_LEAF
uint64_t rbipc_sub_get_lag(const rbipc_sub_t *sub);

#ifdef __cplusplus
}
#endif

#endif /* RBIPC_PUBSUB_H */
