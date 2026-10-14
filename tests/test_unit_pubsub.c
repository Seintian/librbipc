/**
 * @file test_unit_pubsub.c
 * @brief Unit tests for 1-to-N publish-subscribe multicast distribution engine.
 */

#include "rbipc.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
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

static void test_pubsub_from_oldest_and_validation(void) {
    rbipc_ring_t *ring = NULL;
    assert(rbipc_create(NULL, 8, 64, &ring) == RBIPC_OK);

    /* Negative parameter validation */
    rbipc_sub_t *sub = NULL;
    const void *buf = NULL;
    uint32_t len = 0;
    uint64_t seq = 0;

    assert(rbipc_publish(NULL, "a", 1) == RBIPC_ERR_INVAL);
    assert(rbipc_publish(ring, NULL, 1) == RBIPC_ERR_INVAL);
    assert(rbipc_publish(ring, "a", 65) == RBIPC_ERR_INVAL);

    const void *bufs[2] = {"a", "b"};
    uint32_t lens[2] = {1, 1};
    assert(rbipc_publish_batch(NULL, 2, bufs, lens) == RBIPC_ERR_INVAL);
    assert(rbipc_publish_batch(ring, 0, bufs, lens) == RBIPC_ERR_INVAL);
    assert(rbipc_publish_batch(ring, 2, NULL, lens) == RBIPC_ERR_INVAL);
    assert(rbipc_publish_batch(ring, 2, bufs, NULL) == RBIPC_ERR_INVAL);
    const void *null_bufs[2] = {NULL, "b"};
    assert(rbipc_publish_batch(ring, 2, null_bufs, lens) == RBIPC_ERR_INVAL);
    uint32_t bad_lens[2] = {1, 100};
    assert(rbipc_publish_batch(ring, 2, bufs, bad_lens) == RBIPC_ERR_INVAL);

    assert(rbipc_sub_create(NULL, &sub) == RBIPC_ERR_INVAL);
    assert(rbipc_sub_create(ring, NULL) == RBIPC_ERR_INVAL);
    assert(rbipc_sub_create_ex(NULL, true, &sub) == RBIPC_ERR_INVAL);
    assert(rbipc_sub_create_ex(ring, true, NULL) == RBIPC_ERR_INVAL);
    assert(rbipc_sub_poll(NULL, &buf, &len, &seq) == RBIPC_ERR_INVAL);
    assert(rbipc_sub_poll_batch(NULL, 1, &buf, &len, &seq, &len) == RBIPC_ERR_INVAL);
    assert(rbipc_sub_read(NULL, 1000, &buf, &len, &seq) == RBIPC_ERR_INVAL);
    assert(rbipc_sub_get_lag(NULL) == 0);
    rbipc_sub_destroy(NULL);

    /* Test A: Empty ring from_oldest */
    rbipc_sub_t *sub_oldest = NULL;
    assert(rbipc_sub_create_ex(ring, true, &sub_oldest) == RBIPC_OK);
    assert(rbipc_sub_get_lag(sub_oldest) == 0);
    assert(rbipc_sub_poll(sub_oldest, &buf, &len, &seq) == RBIPC_ERR_EMPTY);
    rbipc_sub_destroy(sub_oldest);

    /* Publish 4 messages */
    for (int i = 0; i < 4; ++i) {
        char msg[16];
        snprintf(msg, sizeof(msg), "m%d", i);
        assert(rbipc_publish(ring, msg, (uint32_t)strlen(msg) + 1) == RBIPC_OK);
    }

    /* Test B: Partially filled ring (head=4 <= cap=8) */
    assert(rbipc_sub_create_ex(ring, true, &sub_oldest) == RBIPC_OK);
    assert(rbipc_sub_get_lag(sub_oldest) == 4);

    rbipc_sub_t *sub_latest = NULL;
    assert(rbipc_sub_create_ex(ring, false, &sub_latest) == RBIPC_OK);
    assert(rbipc_sub_get_lag(sub_latest) == 0);
    assert(rbipc_sub_poll(sub_latest, &buf, &len, &seq) == RBIPC_ERR_EMPTY);

    /* Verify sub_oldest reads from 0 */
    assert(rbipc_sub_poll(sub_oldest, &buf, &len, &seq) == RBIPC_OK);
    assert(seq == 0 && strcmp((const char *)buf, "m0") == 0);
    rbipc_sub_destroy(sub_oldest);
    rbipc_sub_destroy(sub_latest);

    /* Publish 10 more messages -> total 14 messages (head=14 > cap=8) */
    for (int i = 4; i < 14; ++i) {
        char msg[16];
        snprintf(msg, sizeof(msg), "m%d", i);
        assert(rbipc_publish(ring, msg, (uint32_t)strlen(msg) + 1) == RBIPC_OK);
    }

    /* Test C: Wrapped ring (oldest available is 14 - 8 = 6) */
    assert(rbipc_sub_create_ex(ring, true, &sub_oldest) == RBIPC_OK);
    assert(rbipc_sub_get_lag(sub_oldest) == 8);
    assert(rbipc_sub_poll(sub_oldest, &buf, &len, &seq) == RBIPC_OK);
    assert(seq == 6 && strcmp((const char *)buf, "m6") == 0);
    rbipc_sub_destroy(sub_oldest);

    /* Test D: Empty poll batch */
    assert(rbipc_sub_create(ring, &sub_latest) == RBIPC_OK);
    uint32_t polled = 999;
    assert(rbipc_sub_poll_batch(sub_latest, 4, &buf, &len, &seq, &polled) == RBIPC_OK);
    assert(polled == 0);
    assert(rbipc_sub_poll_batch(sub_latest, 0, &buf, &len, &seq, &polled) == RBIPC_ERR_INVAL);
    rbipc_sub_destroy(sub_latest);

    rbipc_detach(ring);
}

typedef struct {
    rbipc_sub_t *sub;
    uint64_t timeout_ns;
    int rc;
    char payload[64];
    uint64_t seq;
} pubsub_thread_arg_t;

static void *pubsub_waiter_thread(void *arg) {
    pubsub_thread_arg_t *ctx = (pubsub_thread_arg_t *)arg;
    const void *buf = NULL;
    uint32_t len = 0;
    ctx->rc = rbipc_sub_read(ctx->sub, ctx->timeout_ns, &buf, &len, &ctx->seq);
    if (ctx->rc == RBIPC_OK && buf && len < sizeof(ctx->payload)) {
        memcpy(ctx->payload, buf, len);
        ctx->payload[len] = '\0';
    }
    return NULL;
}

static void test_pubsub_concurrent_wait_wakeup_and_shutdown(void) {
    rbipc_ring_t *ring = NULL;
    assert(rbipc_create(NULL, 16, 128, &ring) == RBIPC_OK);

    rbipc_sub_t *sub = NULL;
    assert(rbipc_sub_create(ring, &sub) == RBIPC_OK);

    const void *buf = NULL;
    uint32_t len = 0;
    uint64_t seq = 0;

    /* 1. Zero timeout on empty topic returns RBIPC_ERR_EMPTY */
    assert(rbipc_sub_read(sub, 0, &buf, &len, &seq) == RBIPC_ERR_EMPTY);

    /* 2. Finite timeout expiration returns RBIPC_ERR_TIMEOUT */
    assert(rbipc_sub_read(sub, 5000000ULL /* 5ms */, &buf, &len, &seq) == RBIPC_ERR_TIMEOUT);

    /* 3. Concurrent waiter wakeup via rbipc_publish */
    pubsub_thread_arg_t arg1 = {
        .sub = sub,
        .timeout_ns = 2000000000ULL, /* 2s */
        .rc = -999,
        .payload = {},
        .seq = 0
    };
    pthread_t th1;
    assert(pthread_create(&th1, NULL, pubsub_waiter_thread, &arg1) == 0);

    /* Give thread time to enter futex wait */
    usleep(20000); /* 20ms */

    const char *wake_msg = "Wakeup multicast payload";
    assert(rbipc_publish(ring, wake_msg, (uint32_t)strlen(wake_msg) + 1) == RBIPC_OK);

    pthread_join(th1, NULL);
    assert(arg1.rc == RBIPC_OK);
    assert(strcmp(arg1.payload, wake_msg) == 0);
    assert(arg1.seq == 0);

    /* 4. Concurrent waiter awakened by shutdown */
    pubsub_thread_arg_t arg2 = {
        .sub = sub,
        .timeout_ns = 2000000000ULL, /* 2s */
        .rc = -999,
        .payload = {},
        .seq = 0
    };
    pthread_t th2;
    assert(pthread_create(&th2, NULL, pubsub_waiter_thread, &arg2) == 0);

    usleep(20000); /* 20ms */

    assert(rbipc_signal_shutdown(ring) == RBIPC_OK);

    pthread_join(th2, NULL);
    assert(arg2.rc == RBIPC_ERR_SHUTDOWN);

    /* 5. Read on shutdown topic returns RBIPC_ERR_SHUTDOWN immediately */
    assert(rbipc_sub_read(sub, 1000000ULL, &buf, &len, &seq) == RBIPC_ERR_SHUTDOWN);

    rbipc_sub_destroy(sub);
    rbipc_detach(ring);
}

int main(void) {
    printf("[test_unit_pubsub] Running 1-to-N multicast publish-subscribe tests...\n");
    test_pubsub_basic_multicast();
    test_pubsub_lapped_subscriber();
    test_pubsub_batch_and_streaming();
    test_pubsub_from_oldest_and_validation();
    test_pubsub_concurrent_wait_wakeup_and_shutdown();
    printf("[test_unit_pubsub] All tests passed!\n");
    return 0;
}
