/**
 * @file deep_pubsub.c
 * @brief 1-to-N multicast publish-subscribe fanout engine micro-benchmark
 */

#include "deep_common.h"

typedef struct {
    rbipc_sub_t *sub;
    int expected_msgs;
} sub_thread_arg_t;

static void *pubsub_subscriber_worker(void *arg) {
    sub_thread_arg_t *a = (sub_thread_arg_t *)arg;
    uint64_t last_seq = 0;
    while (last_seq < (uint64_t)a->expected_msgs) {
        const void *msg = NULL;
        uint32_t len = 0;
        uint64_t seq = 0;
        int rc = rbipc_sub_read(a->sub, 200000000ULL, &msg, &len, &seq);
        if (rc == RBIPC_OK) {
            last_seq = seq + 1;
        } else if (rc == RBIPC_ERR_SHUTDOWN || rc == RBIPC_ERR_TIMEOUT) {
            break;
        }
    }
    return NULL;
}

void run_pubsub_fanout_bench(uint32_t num_subs, int iterations, pubsub_fanout_bench_t *out) {
    rbipc_destroy(DEEP_PUBSUB);
    rbipc_ring_t *ring = NULL;
    assert(rbipc_create(DEEP_PUBSUB, 4096, 64, &ring) == RBIPC_OK);

    rbipc_sub_t *subs[8];
    for (uint32_t i = 0; i < num_subs; ++i) {
        assert(rbipc_sub_create(ring, &subs[i]) == RBIPC_OK);
    }

    pthread_t threads[8];
    sub_thread_arg_t args[8];
    for (uint32_t i = 0; i < num_subs; ++i) {
        args[i].sub = subs[i];
        args[i].expected_msgs = iterations;
        pthread_create(&threads[i], NULL, pubsub_subscriber_worker, &args[i]);
    }

    uint64_t t0 = get_time_ns();
    for (int i = 0; i < iterations; ++i) {
        uint64_t payload = (uint64_t)i;
        int rc = rbipc_publish(ring, &payload, sizeof(payload));
        assert(rc == RBIPC_OK);
    }

    for (uint32_t i = 0; i < num_subs; ++i) {
        pthread_join(threads[i], NULL);
        rbipc_sub_destroy(subs[i]);
    }
    uint64_t t1 = get_time_ns();

    double sec = (double)(t1 - t0) / (double)RBIPC_NSEC_PER_SEC;
    out->num_subscribers         = num_subs;
    out->msgs_per_sec            = (double)iterations / sec;
    out->mb_per_sec              = ((double)iterations * sizeof(uint64_t)) / (1024.0 * 1024.0 * sec);
    out->effective_delivery_rate = ((double)iterations * num_subs) / sec;

    rbipc_detach(ring);
    rbipc_destroy(DEEP_PUBSUB);
}
