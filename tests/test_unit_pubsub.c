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

int main(void) {
    printf("[test_unit_pubsub] Running 1-to-N multicast publish-subscribe tests...\n");
    test_pubsub_basic_multicast();
    test_pubsub_lapped_subscriber();
    printf("[test_unit_pubsub] All tests passed!\n");
    return 0;
}
