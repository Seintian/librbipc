/**
 * @file comp_dpdk.c
 * @brief Standalone DPDK rte_ring MPMC lockless ring buffer implementation.
 * @details Reference: dpdk/lib/ring/rte_ring_c11_pvt.h
 */

#include "comp_common.h"

struct rte_ring {
    alignas(CACHE_LINE_SIZE) atomic_uint prod_head;
    alignas(CACHE_LINE_SIZE) atomic_uint prod_tail;
    alignas(CACHE_LINE_SIZE) atomic_uint cons_head;
    alignas(CACHE_LINE_SIZE) atomic_uint cons_tail;
    uint32_t mask;
    uint32_t capacity;
};

rte_ring_t *rte_ring_alloc(uint32_t cap) {
    assert((cap & (cap - 1)) == 0);
    size_t total = sizeof(rte_ring_t) + cap * sizeof(uintptr_t);
    void *mem = mmap(NULL, total, PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) return NULL;
    rte_ring_t *r = (rte_ring_t *)mem;
    atomic_init(&r->prod_head, 0); atomic_init(&r->prod_tail, 0);
    atomic_init(&r->cons_head, 0); atomic_init(&r->cons_tail, 0);
    r->mask = cap - 1; r->capacity = cap;
    return r;
}

void rte_ring_free(rte_ring_t *r) {
    if (r) munmap(r, sizeof(rte_ring_t) + r->capacity * sizeof(uintptr_t));
}

static inline uintptr_t *rte_slots(rte_ring_t *r) {
    return (uintptr_t *)((char *)r + sizeof(rte_ring_t));
}

int rte_mp_enqueue(rte_ring_t *r, uintptr_t val) {
    uint32_t ph, pn, ct;
    for (;;) {
        ph = atomic_load_explicit(&r->prod_head, memory_order_relaxed);
        ct = atomic_load_explicit(&r->cons_tail, memory_order_acquire);
        pn = ph + 1;
        if (r->mask + ct - ph == 0) return -ENOBUFS;
        if (atomic_compare_exchange_weak_explicit(&r->prod_head, &ph, pn,
                memory_order_acquire, memory_order_relaxed)) break;
    }
    rte_slots(r)[ph & r->mask] = val;
    uint32_t tail_spins = 0;
    while (atomic_load_explicit(&r->prod_tail, memory_order_relaxed) != ph)
        bench_spin_pause(&tail_spins);
    atomic_store_explicit(&r->prod_tail, pn, memory_order_release);
    return 0;
}

int rte_mc_dequeue(rte_ring_t *r, uintptr_t *out) {
    uint32_t ch, cn, pt;
    for (;;) {
        ch = atomic_load_explicit(&r->cons_head, memory_order_relaxed);
        pt = atomic_load_explicit(&r->prod_tail, memory_order_acquire);
        if (ch == pt) return -ENOENT;
        cn = ch + 1;
        if (atomic_compare_exchange_weak_explicit(&r->cons_head, &ch, cn,
                memory_order_acquire, memory_order_relaxed)) break;
    }
    *out = rte_slots(r)[ch & r->mask];
    uint32_t cons_spins = 0;
    while (atomic_load_explicit(&r->cons_tail, memory_order_relaxed) != ch)
        bench_spin_pause(&cons_spins);
    atomic_store_explicit(&r->cons_tail, cn, memory_order_release);
    return 0;
}

void rte_enqueue_wait(rte_ring_t *r, uintptr_t v) {
    uint32_t spins = 0;
    while (rte_mp_enqueue(r, v) == -ENOBUFS) bench_spin_pause(&spins);
}

void rte_dequeue_wait(rte_ring_t *r, uintptr_t *v) {
    uint32_t spins = 0;
    while (rte_mc_dequeue(r, v) == -ENOENT) bench_spin_pause(&spins);
}

uint32_t rte_mp_enqueue_burst(rte_ring_t *r, const uintptr_t *vals, uint32_t n) {
    uint32_t ph, pn, ct, free_entries;
    for (;;) {
        ph = atomic_load_explicit(&r->prod_head, memory_order_relaxed);
        ct = atomic_load_explicit(&r->cons_tail, memory_order_acquire);
        free_entries = r->mask + ct - ph;
        if (free_entries == 0) return 0;
        uint32_t to_enq = (n < free_entries) ? n : free_entries;
        pn = ph + to_enq;
        if (atomic_compare_exchange_weak_explicit(&r->prod_head, &ph, pn,
                memory_order_acquire, memory_order_relaxed)) {
            for (uint32_t i = 0; i < to_enq; i++) {
                rte_slots(r)[(ph + i) & r->mask] = vals[i];
            }
            uint32_t tail_spins = 0;
            while (atomic_load_explicit(&r->prod_tail, memory_order_relaxed) != ph)
                bench_spin_pause(&tail_spins);
            atomic_store_explicit(&r->prod_tail, pn, memory_order_release);
            return to_enq;
        }
    }
}

uint32_t rte_mc_dequeue_burst(rte_ring_t *r, uintptr_t *out, uint32_t n) {
    uint32_t ch, cn, pt, entries;
    for (;;) {
        ch = atomic_load_explicit(&r->cons_head, memory_order_relaxed);
        pt = atomic_load_explicit(&r->prod_tail, memory_order_acquire);
        entries = pt - ch;
        if (entries == 0) return 0;
        uint32_t to_deq = (n < entries) ? n : entries;
        cn = ch + to_deq;
        if (atomic_compare_exchange_weak_explicit(&r->cons_head, &ch, cn,
                memory_order_acquire, memory_order_relaxed)) {
            for (uint32_t i = 0; i < to_deq; i++) {
                out[i] = rte_slots(r)[(ch + i) & r->mask];
            }
            uint32_t cons_spins = 0;
            while (atomic_load_explicit(&r->cons_tail, memory_order_relaxed) != ch)
                bench_spin_pause(&cons_spins);
            atomic_store_explicit(&r->cons_tail, cn, memory_order_release);
            return to_deq;
        }
    }
}
