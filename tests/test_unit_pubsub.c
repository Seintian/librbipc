/**
 * @file test_unit_pubsub.c
 * @brief Unit tests for 1-to-N publish-subscribe multicast distribution engine.
 */

#include "rbipc.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

static void test_pubsub_basic_multicast(void) {
    rbipc_ring_t *ring = NULL;
    int rc = rbipc_create(NULL, 16, 256, &ring);
    assert(rc == RBIPC_OK);
    assert(ring != NULL);

    /* Create 3 independent subscribers */
    rbipc_sub_t *sub1 = NULL;
    rbipc_sub_t *sub2 = NULL;
    rbipc_sub_t *sub3 = NULL;

    assert(rbipc_sub_create(ring, &sub1) == RBIPC_OK);
    assert(rbipc_sub_create(ring, &sub2) == RBIPC_OK);
    assert(rbipc_sub_create(ring, &sub3) == RBIPC_OK);

    /* Initially no messages */
    const void *buf = NULL;
    uint32_t len = 0;
    uint64_t seq = 0;
    assert(rbipc_sub_poll(sub1, &buf, &len, &seq) == RBIPC_ERR_EMPTY);

    /* Publish 10 messages */
    for (int i = 0; i < 10; ++i) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Multicast message %d", i);
        assert(rbipc_publish(ring, msg, (uint32_t)strlen(msg) + 1) == RBIPC_OK);
    }

    /* All 3 subscribers must see lag == 10 */
    assert(rbipc_sub_get_lag(sub1) == 10);
    assert(rbipc_sub_get_lag(sub2) == 10);
    assert(rbipc_sub_get_lag(sub3) == 10);

    /* Subscriber 1 reads all 10 messages */
    for (int i = 0; i < 10; ++i) {
        char expected[64];
        snprintf(expected, sizeof(expected), "Multicast message %d", i);

        assert(rbipc_sub_poll(sub1, &buf, &len, &seq) == RBIPC_OK);
        assert(seq == (uint64_t)i);
        assert(strcmp((const char *)buf, expected) == 0);
    }
    assert(rbipc_sub_get_lag(sub1) == 0);

    /* Subscriber 2 reads all 10 messages using timed wait */
    for (int i = 0; i < 10; ++i) {
        char expected[64];
        snprintf(expected, sizeof(expected), "Multicast message %d", i);

        assert(rbipc_sub_read(sub2, 1000000ULL, &buf, &len, &seq) == RBIPC_OK);
        assert(seq == (uint64_t)i);
        assert(strcmp((const char *)buf, expected) == 0);
    }
    assert(rbipc_sub_get_lag(sub2) == 0);

    /* Subscriber 3 reads all 10 messages */
    for (int i = 0; i < 10; ++i) {
        assert(rbipc_sub_poll(sub3, &buf, &len, &seq) == RBIPC_OK);
        assert(seq == (uint64_t)i);
    }
    assert(rbipc_sub_get_lag(sub3) == 0);

    rbipc_sub_destroy(sub1);
    rbipc_sub_destroy(sub2);
    rbipc_sub_destroy(sub3);
    rbipc_detach(ring);
}

static void test_pubsub_lapped_subscriber(void) {
    /* Capacity 4 topic */
    rbipc_ring_t *ring = NULL;
    assert(rbipc_create(NULL, 4, 128, &ring) == RBIPC_OK);

    rbipc_sub_t *sub = NULL;
    assert(rbipc_sub_create(ring, &sub) == RBIPC_OK);

    /* Publish 10 messages, wrapping the 4-slot ring multiple times */
    for (int i = 0; i < 10; ++i) {
        char msg[32];
        snprintf(msg, sizeof(msg), "msg %d", i);
        assert(rbipc_publish(ring, msg, (uint32_t)strlen(msg) + 1) == RBIPC_OK);
    }

    /* Subscriber was lapped: oldest available in 4-slot ring is message 6 (10 - 4 = 6) */
    const void *buf = NULL;
    uint32_t len = 0;
    uint64_t seq = 0;

    assert(rbipc_sub_poll(sub, &buf, &len, &seq) == RBIPC_OK);
    assert(seq == 6);
    assert(strcmp((const char *)buf, "msg 6") == 0);

    rbipc_sub_destroy(sub);
    rbipc_detach(ring);
}

static void test_pubsub_batch_and_streaming(void) {
    /* Create 32-slot topic with 512-byte slot size */
    rbipc_ring_t *ring = NULL;
    assert(rbipc_create(NULL, 32, 512, &ring) == RBIPC_OK);

    rbipc_sub_t *sub = NULL;
    assert(rbipc_sub_create(ring, &sub) == RBIPC_OK);

    /* 1. Test publish_batch with 8 messages */
    char payloads[8][64];
    const void *bufs[8];
    uint32_t lens[8];
    for (int i = 0; i < 8; ++i) {
        snprintf(payloads[i], sizeof(payloads[i]), "Batch PubSub Item #%d", i);
        bufs[i] = payloads[i];
        lens[i] = (uint32_t)strlen(payloads[i]) + 1;
    }

    assert(rbipc_publish_batch(ring, 8, bufs, lens) == RBIPC_OK);
    assert(rbipc_sub_get_lag(sub) == 8);

    /* 2. Test sub_poll_batch: poll all 8 messages in a single call */
    const void *polled_bufs[8];
    uint32_t polled_lens[8];
    uint64_t polled_seqs[8];
    uint32_t polled_count = 0;

    assert(rbipc_sub_poll_batch(sub, 8, polled_bufs, polled_lens, polled_seqs, &polled_count) == RBIPC_OK);
    assert(polled_count == 8);
    for (uint32_t i = 0; i < 8; ++i) {
        assert(polled_seqs[i] == (uint64_t)i);
        assert(polled_lens[i] == lens[i]);
        assert(strcmp((const char *)polled_bufs[i], payloads[i]) == 0);
    }
    assert(rbipc_sub_get_lag(sub) == 0);

    /* 3. Test large payload (>= 256 bytes) non-temporal streaming copy */
    char large_payload[384];
    memset(large_payload, 0x5A, sizeof(large_payload));
    large_payload[sizeof(large_payload) - 1] = '\0';

    assert(rbipc_publish(ring, large_payload, sizeof(large_payload)) == RBIPC_OK);
    assert(rbipc_sub_get_lag(sub) == 1);

    const void *recv_large = NULL;
    uint32_t recv_len = 0;
    uint64_t recv_seq = 0;
    assert(rbipc_sub_poll(sub, &recv_large, &recv_len, &recv_seq) == RBIPC_OK);
    assert(recv_len == sizeof(large_payload));
    assert(recv_seq == 8);
    assert(memcmp(recv_large, large_payload, sizeof(large_payload)) == 0);

    rbipc_sub_destroy(sub);
    rbipc_detach(ring);
}

int main(void) {
    printf("[test_unit_pubsub] Running 1-to-N multicast publish-subscribe tests...\n");
    test_pubsub_basic_multicast();
    test_pubsub_lapped_subscriber();
    test_pubsub_batch_and_streaming();
    printf("[test_unit_pubsub] All tests passed!\n");
    return 0;
}
