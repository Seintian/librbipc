/**
 * @file comp_disruptor.c
 * @brief Standalone LMAX Disruptor (WorkProcessor / WorkerPool) implementation.
 * @details Reference: Disruptor-cpp/Disruptor/WorkProcessor.h
 */

#include "comp_common.h"

struct disruptor_ring {
    alignas(CACHE_LINE_SIZE) _Atomic int64_t prod_cursor;
    alignas(CACHE_LINE_SIZE) _Atomic int64_t prod_claim;
    alignas(CACHE_LINE_SIZE) _Atomic int64_t work_sequence;
    alignas(CACHE_LINE_SIZE) _Atomic int64_t cons_gating;
    uint32_t capacity;
    uint32_t mask;
};

disruptor_ring_t *disruptor_alloc(uint32_t cap) {
    assert((cap & (cap - 1)) == 0);
    size_t sz = sizeof(disruptor_ring_t) + cap * sizeof(uintptr_t);
    void *m = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) return NULL;
    disruptor_ring_t *d = (disruptor_ring_t *)m;
    atomic_init(&d->prod_cursor, -1);
    atomic_init(&d->prod_claim, -1);
    atomic_init(&d->work_sequence, -1);
    atomic_init(&d->cons_gating, -1);
    d->capacity = cap;
    d->mask = cap - 1;
    return d;
}

void disruptor_free(disruptor_ring_t *d) {
    if (d) munmap(d, sizeof(disruptor_ring_t) + d->capacity * sizeof(uintptr_t));
}

static inline uintptr_t *disruptor_slots(disruptor_ring_t *d) {
    return (uintptr_t *)((char *)d + sizeof(disruptor_ring_t));
}

void disruptor_publish_sp(disruptor_ring_t *d, int64_t seq, uintptr_t val) {
    uint32_t spins = 0;
    while (seq - atomic_load_explicit(&d->cons_gating, memory_order_acquire) >= (int64_t)d->capacity) {
        bench_spin_pause(&spins);
    }
    disruptor_slots(d)[(uint32_t)seq & d->mask] = val;
    atomic_store_explicit(&d->prod_cursor, seq, memory_order_release);
}

void disruptor_publish_mp(disruptor_ring_t *d, uintptr_t val) {
    int64_t cur, next;
    uint32_t spins = 0;
    do {
        cur = atomic_load_explicit(&d->prod_claim, memory_order_relaxed);
        next = cur + 1;
        while (next - atomic_load_explicit(&d->cons_gating, memory_order_acquire) >= (int64_t)d->capacity) {
            bench_spin_pause(&spins);
        }
    } while (!atomic_compare_exchange_weak_explicit(&d->prod_claim, &cur, next,
                                                    memory_order_acq_rel, memory_order_relaxed));

    disruptor_slots(d)[(uint32_t)next & d->mask] = val;
    spins = 0;
    while (atomic_load_explicit(&d->prod_cursor, memory_order_acquire) != next - 1) {
        bench_spin_pause(&spins);
    }
    atomic_store_explicit(&d->prod_cursor, next, memory_order_release);
}

bool disruptor_consume_wp(disruptor_ring_t *d, int64_t max_seq, uintptr_t *out) {
    int64_t cur, next;
    do {
        cur = atomic_load_explicit(&d->work_sequence, memory_order_relaxed);
        next = cur + 1;
        if (max_seq >= 0 && next >= max_seq) return false;
    } while (!atomic_compare_exchange_weak_explicit(&d->work_sequence, &cur, next,
                                                    memory_order_acq_rel, memory_order_relaxed));

    uint32_t spins = 0;
    while (atomic_load_explicit(&d->prod_cursor, memory_order_acquire) < next) {
        bench_spin_pause(&spins);
    }
    *out = disruptor_slots(d)[(uint32_t)next & d->mask];
    spins = 0;
    while (atomic_load_explicit(&d->cons_gating, memory_order_acquire) != next - 1) {
        bench_spin_pause(&spins);
    }
    atomic_store_explicit(&d->cons_gating, next, memory_order_release);
    return true;
}

uint32_t disruptor_publish_batch(disruptor_ring_t *d, const uintptr_t *vals, uint32_t n) {
    int64_t cur, next;
    uint32_t spins = 0;
    do {
        cur = atomic_load_explicit(&d->prod_claim, memory_order_relaxed);
        next = cur + (int64_t)n;
        while (next - atomic_load_explicit(&d->cons_gating, memory_order_acquire) >= (int64_t)d->capacity) {
            bench_spin_pause(&spins);
        }
    } while (!atomic_compare_exchange_weak_explicit(&d->prod_claim, &cur, next,
                                                    memory_order_acq_rel, memory_order_relaxed));

    for (uint32_t i = 0; i < n; i++) {
        disruptor_slots(d)[(uint32_t)(cur + 1 + (int64_t)i) & d->mask] = vals[i];
    }
    spins = 0;
    while (atomic_load_explicit(&d->prod_cursor, memory_order_acquire) != cur) {
        bench_spin_pause(&spins);
    }
    atomic_store_explicit(&d->prod_cursor, next, memory_order_release);
    return n;
}

uint32_t disruptor_consume_batch(disruptor_ring_t *d, int64_t max_seq, uintptr_t *out, uint32_t max_n) {
    int64_t cur, next;
    uint32_t to_consume = 0;
    do {
        cur = atomic_load_explicit(&d->work_sequence, memory_order_relaxed);
        int64_t prod = atomic_load_explicit(&d->prod_cursor, memory_order_acquire);
        if (cur >= prod) return 0;
        int64_t avail = prod - cur;
        to_consume = (avail < (int64_t)max_n) ? (uint32_t)avail : max_n;
        next = cur + (int64_t)to_consume;
        if (max_seq >= 0 && next > max_seq) {
            to_consume = (uint32_t)(max_seq - cur);
            next = max_seq;
            if (to_consume == 0) return 0;
        }
    } while (!atomic_compare_exchange_weak_explicit(&d->work_sequence, &cur, next,
                                                    memory_order_acq_rel, memory_order_relaxed));

    for (uint32_t i = 0; i < to_consume; i++) {
        out[i] = disruptor_slots(d)[(uint32_t)(cur + 1 + (int64_t)i) & d->mask];
    }
    uint32_t spins = 0;
    while (atomic_load_explicit(&d->cons_gating, memory_order_acquire) != cur) {
        bench_spin_pause(&spins);
    }
    atomic_store_explicit(&d->cons_gating, next, memory_order_release);
    return to_consume;
}
