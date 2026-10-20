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
