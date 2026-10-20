/**
 * @file opt_pubsub.c
 * @brief Module 5: Multicast PubSub Zero-Copy vs Standard Copy.
 */

#include "arch_common.h"

typedef struct {
    rbipc_sub_t *sub;
    uint64_t expected_msgs;
} arch_sub_worker_arg_t;

static void *arch_pubsub_worker_func(void *arg) {
    arch_sub_worker_arg_t *w = (arch_sub_worker_arg_t *)arg;
    uint64_t last_seq = 0;
    while (last_seq < w->expected_msgs) {
        const void *buf = NULL;
        uint32_t len = 0;
        uint64_t seq = 0;
        int rc = rbipc_sub_read(w->sub, 200000000ULL, &buf, &len, &seq);
        if (rc == RBIPC_OK) {
            last_seq = seq + 1;
        } else if (rc == RBIPC_ERR_SHUTDOWN || rc == RBIPC_ERR_TIMEOUT) {
            break;
        }
    }
    return NULL;
}

void run_module5_pubsub(uint64_t iterations, m5_pubsub_result_t results[16]) {
    arch_print_subhdr("Module 5: Multicast PubSub Zero-Copy vs Standard Copy");
    printf("Messages: %lu | Fanouts: 1, 2, 4, 8 Subscribers\n\n", (unsigned long)iterations);

    const uint32_t fanouts[4] = { 1, 2, 4, 8 };
    const struct {
        const char *name;
        bool is_zero_copy;
        uint32_t flags;
    } configs[4] = {
        { "Standard Copy (Std Ring)",     false, 0 },
        { "Standard Copy (Packed Ring)",  false, RBIPC_FLAG_PACKED },
        { "Zero-Copy (Std Ring)",         true,  0 },
        { "Zero-Copy (Packed Ring)",      true,  RBIPC_FLAG_PACKED }
    };

    int idx = 0;
    for (int c = 0; c < 4; ++c) {
        for (int f = 0; f < 4; ++f) {
            uint32_t fanout = fanouts[f];
            results[idx].mode_name = configs[c].name;
            results[idx].fanout = fanout;

            rbipc_destroy(ARCH_SHM_PUBSUB);
            rbipc_ring_t *ring = NULL;
            assert(rbipc_create_ex(ARCH_SHM_PUBSUB, 16384, 64,
                                  RBIPC_SHM_NAMED | configs[c].flags, 0660, &ring) == RBIPC_OK);

            rbipc_sub_t *subs[8];
            pthread_t threads[8];
            arch_sub_worker_arg_t args[8];

            for (uint32_t s = 0; s < fanout; ++s) {
                assert(rbipc_sub_create(ring, &subs[s]) == RBIPC_OK);
                args[s].sub = subs[s];
                args[s].expected_msgs = iterations;
                pthread_create(&threads[s], NULL, arch_pubsub_worker_func, &args[s]);
            }

            uint64_t t0 = arch_get_time_ns();
            if (!configs[c].is_zero_copy) {
                for (uint64_t i = 0; i < iterations; ++i) {
                    uint64_t val = i;
                    assert(rbipc_publish(ring, &val, sizeof(val)) == RBIPC_OK);
                }
            } else {
                for (uint64_t i = 0; i < iterations; ++i) {
                    void *wbuf = NULL;
                    uint64_t ticket = 0;
                    assert(rbipc_publish_reserve(ring, sizeof(uint64_t), &wbuf, &ticket) == RBIPC_OK);
                    *(uint64_t *)wbuf = i;
                    assert(rbipc_publish_commit(ring, ticket, sizeof(uint64_t)) == RBIPC_OK);
                }
            }

            for (uint32_t s = 0; s < fanout; ++s) {
                pthread_join(threads[s], NULL);
                rbipc_sub_destroy(subs[s]);
            }
            uint64_t t1 = arch_get_time_ns();

            rbipc_detach(ring);
            rbipc_destroy(ARCH_SHM_PUBSUB);

            double sec = (double)(t1 - t0) / 1e9;
            results[idx].pub_msgs_sec       = (double)iterations / sec;
            results[idx].effective_msgs_sec = ((double)iterations * fanout) / sec;
            results[idx].effective_mb_sec   = (results[idx].effective_msgs_sec * 64.0) / (1024.0 * 1024.0);

            printf("  %-28s (Fanout: %u) : Publish: %10.2f msgs/s | Effective: %10.2f msgs/s (%7.2f MB/s)\n",
                   results[idx].mode_name, fanout, results[idx].pub_msgs_sec,
                   results[idx].effective_msgs_sec, results[idx].effective_mb_sec);
            idx++;
        }
        printf("\n");
    }
}
