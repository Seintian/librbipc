/**
 * @file rbipc.c
 * @brief Implementation of librbipc (Lock-Free Shared-Memory Ring Buffer IPC)
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "rbipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <linux/futex.h>
#include <time.h>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
static inline void cpu_pause(void) {
    _mm_pause();
}
#elif defined(__aarch64__) || defined(__arm__)
static inline void cpu_pause(void) {
    __asm__ __volatile__("isb\n" ::: "memory");
}
#else
static inline void cpu_pause(void) {
    /* Portable no-op fallback */
}
#endif

#ifndef F_LINUX_SPECIFIC_BASE
#define F_LINUX_SPECIFIC_BASE 1024
#endif
#ifndef F_ADD_SEALS
#define F_ADD_SEALS (F_LINUX_SPECIFIC_BASE + 9)
#define F_GET_SEALS (F_LINUX_SPECIFIC_BASE + 10)
#define F_SEAL_SEAL   0x0001
#define F_SEAL_SHRINK 0x0002
#define F_SEAL_GROW   0x0004
#endif

#define DEFAULT_SPIN_THRESHOLD 50000
#define SPIN_TIER_PAUSE         2000
#define SPIN_TIER_YIELD         2050

/* ============================================================================
 * Internal Ring Handle Definition
 * ============================================================================ */

struct rbipc_ring {
    int fd;                       /**< Shared memory file descriptor */
    char name[256];               /**< POSIX shared memory name */
    rbipc_shm_header_t *hdr;      /**< Pointer to control header */
    rbipc_slot_t *slots;          /**< Pointer to slot descriptors array */
    void *ctrl_map;               /**< Base virtual pointer to metadata map */
    size_t ctrl_map_size;         /**< Size of metadata map (page aligned) */
    void *data_map;               /**< Contiguous 2x double-mapped virtual buffer */
    size_t data_size;             /**< Size of single buffer */
    bool is_creator;              /**< True if created by this process */
};

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

static inline uint32_t round_up_pow2_32(uint32_t v) {
    if (v == 0) return 1;
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    return v + 1;
}

static inline size_t align_up(size_t val, size_t alignment) {
    return (val + alignment - 1) & ~(alignment - 1);
}

static inline int futex_wait(_Atomic uint32_t *uaddr, uint32_t val, const struct timespec *timeout) {
    /* Process-shared futex wait (FUTEX_WAIT without FUTEX_PRIVATE_FLAG) */
    return (int)syscall(SYS_futex, (uint32_t *)uaddr, FUTEX_WAIT, val, timeout, NULL, 0);
}

static inline int futex_wake(_Atomic uint32_t *uaddr, int count) {
    /* Process-shared futex wake */
    return (int)syscall(SYS_futex, (uint32_t *)uaddr, FUTEX_WAKE, count, NULL, NULL, 0);
}

/**
 * @brief Double-maps a shared memory file region into contiguous virtual address space.
 */
static int map_double_ring(int fd, size_t offset, size_t data_size, void **out_data_map) {
    /* 1. Reserve 2 * data_size contiguous virtual address space */
    void *anon = mmap(NULL, 2 * data_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (anon == MAP_FAILED) {
        return RBIPC_ERR_NOMEM;
    }

    /* 2. Map first half directly to the shared memory data region */
    void *m1 = mmap(anon, data_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, (off_t)offset);
    if (m1 == MAP_FAILED) {
        munmap(anon, 2 * data_size);
        return RBIPC_ERR_SYS;
    }

    /* 3. Map second half to the EXACT SAME shared memory data region */
    void *m2 = mmap((char *)anon + data_size, data_size, PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_FIXED, fd, (off_t)offset);
    if (m2 == MAP_FAILED) {
        munmap(anon, 2 * data_size);
        return RBIPC_ERR_SYS;
    }

    *out_data_map = anon;
    return RBIPC_OK;
}

/* ============================================================================
 * Lifecycle & Management API
 * ============================================================================ */

int rbipc_create(const char *name, size_t capacity, uint32_t slot_size, rbipc_ring_t **out_ring) {
    if (!out_ring || capacity == 0 || slot_size == 0) {
        return RBIPC_ERR_INVAL;
    }

    long page_size_raw = sysconf(_SC_PAGESIZE);
    if (page_size_raw <= 0) return RBIPC_ERR_SYS;
    size_t page_size = (size_t)page_size_raw;

    uint32_t cap = round_up_pow2_32((uint32_t)capacity);
    if (cap < 2) cap = 2;

    /* Align slot size to 64 bytes for cache line efficiency */
    size_t aligned_slot_size = align_up(slot_size, RBIPC_CACHE_LINE);
    size_t raw_data_size = (size_t)cap * aligned_slot_size;
    size_t data_size = align_up(raw_data_size, page_size);

    size_t header_and_slots = sizeof(rbipc_shm_header_t) + (size_t)cap * sizeof(rbipc_slot_t);
    size_t data_offset = align_up(header_and_slots, page_size);
    size_t total_shm_size = data_offset + data_size;

    int fd = -1;
    if (name) {
        /* Remove any preexisting object to start clean */
        shm_unlink(name);
        fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0666);
    } else {
        fd = memfd_create("rbipc_anon", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    }

    if (fd < 0) {
        return RBIPC_ERR_SYS;
    }

    if (ftruncate(fd, (off_t)total_shm_size) != 0) {
        int err = errno;
        close(fd);
        if (name) shm_unlink(name);
        errno = err;
        return RBIPC_ERR_SYS;
    }

    /* Apply file sealing against accidental resizing */
#ifdef F_ADD_SEALS
    (void)fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW);
#endif

    /* Map control metadata */
    void *ctrl_map = mmap(NULL, data_offset, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ctrl_map == MAP_FAILED) {
        close(fd);
        if (name) shm_unlink(name);
        return RBIPC_ERR_SYS;
    }

    /* Initialize Header */
    rbipc_shm_header_t *hdr = (rbipc_shm_header_t *)ctrl_map;
    memset(hdr, 0, sizeof(*hdr));
    hdr->magic = RBIPC_MAGIC;
    hdr->version = RBIPC_VERSION;
    hdr->header_size = sizeof(*hdr);
    hdr->total_shm_size = (uint64_t)total_shm_size;
    hdr->data_offset = (uint64_t)data_offset;
    hdr->data_size = (uint64_t)data_size;
    hdr->capacity = cap;
    hdr->capacity_mask = cap - 1;
    hdr->slot_size = (uint32_t)aligned_slot_size;
    hdr->spin_threshold = DEFAULT_SPIN_THRESHOLD;

    atomic_init(&hdr->write_ticket, 0);
    atomic_init(&hdr->read_ticket, 0);
    atomic_init(&hdr->futex_seq, 0);
    atomic_init(&hdr->active_producers, 1);
    atomic_init(&hdr->active_consumers, 0);
    atomic_init(&hdr->shutdown_flag, 0);

    /* Initialize Slots */
    rbipc_slot_t *slots = (rbipc_slot_t *)((char *)ctrl_map + sizeof(rbipc_shm_header_t));
    for (uint32_t i = 0; i < cap; ++i) {
        atomic_init(&slots[i].sequence, i);
        atomic_init(&slots[i].state, RBIPC_SLOT_EMPTY);
        atomic_init(&slots[i].producer_pid, 0);
        atomic_init(&slots[i].len, 0);
    }

    /* Setup Double-Mapped Virtual Buffer */
    void *data_map = NULL;
    int rc = map_double_ring(fd, data_offset, data_size, &data_map);
    if (rc != RBIPC_OK) {
        munmap(ctrl_map, data_offset);
        close(fd);
        if (name) shm_unlink(name);
        return rc;
    }

    rbipc_ring_t *ring = (rbipc_ring_t *)calloc(1, sizeof(rbipc_ring_t));
    if (!ring) {
        munmap(data_map, 2 * data_size);
        munmap(ctrl_map, data_offset);
        close(fd);
        if (name) shm_unlink(name);
        return RBIPC_ERR_NOMEM;
    }

    ring->fd = fd;
    if (name) {
        strncpy(ring->name, name, sizeof(ring->name) - 1);
    }
    ring->hdr = hdr;
    ring->slots = slots;
    ring->ctrl_map = ctrl_map;
    ring->ctrl_map_size = data_offset;
    ring->data_map = data_map;
    ring->data_size = data_size;
    ring->is_creator = true;

    *out_ring = ring;
    return RBIPC_OK;
}

int rbipc_attach(const char *name, rbipc_ring_t **out_ring) {
    if (!name || !out_ring) {
        return RBIPC_ERR_INVAL;
    }

    int fd = shm_open(name, O_RDWR, 0666);
    if (fd < 0) {
        return RBIPC_ERR_SYS;
    }

    /* Map minimal page to inspect header */
    long page_size_raw = sysconf(_SC_PAGESIZE);
    if (page_size_raw <= 0) {
        close(fd);
        return RBIPC_ERR_SYS;
    }
    size_t page_size = (size_t)page_size_raw;

    void *init_map = mmap(NULL, page_size, PROT_READ, MAP_SHARED, fd, 0);
    if (init_map == MAP_FAILED) {
        close(fd);
        return RBIPC_ERR_SYS;
    }

    rbipc_shm_header_t *hdr_check = (rbipc_shm_header_t *)init_map;
    if (hdr_check->magic != RBIPC_MAGIC || hdr_check->version != RBIPC_VERSION) {
        munmap(init_map, page_size);
        close(fd);
        return RBIPC_ERR_INVAL;
    }

    size_t data_offset = (size_t)hdr_check->data_offset;
    size_t data_size = (size_t)hdr_check->data_size;
    munmap(init_map, page_size);

    /* Map full control header and slot table */
    void *ctrl_map = mmap(NULL, data_offset, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ctrl_map == MAP_FAILED) {
        close(fd);
        return RBIPC_ERR_SYS;
    }

    rbipc_shm_header_t *hdr = (rbipc_shm_header_t *)ctrl_map;
    rbipc_slot_t *slots = (rbipc_slot_t *)((char *)ctrl_map + sizeof(rbipc_shm_header_t));

    /* Map 2x virtual ring */
    void *data_map = NULL;
    int rc = map_double_ring(fd, data_offset, data_size, &data_map);
    if (rc != RBIPC_OK) {
        munmap(ctrl_map, data_offset);
        close(fd);
        return rc;
    }

    rbipc_ring_t *ring = (rbipc_ring_t *)calloc(1, sizeof(rbipc_ring_t));
    if (!ring) {
        munmap(data_map, 2 * data_size);
        munmap(ctrl_map, data_offset);
        close(fd);
        return RBIPC_ERR_NOMEM;
    }

    ring->fd = fd;
    strncpy(ring->name, name, sizeof(ring->name) - 1);
    ring->hdr = hdr;
    ring->slots = slots;
    ring->ctrl_map = ctrl_map;
    ring->ctrl_map_size = data_offset;
    ring->data_map = data_map;
    ring->data_size = data_size;
    ring->is_creator = false;

    atomic_fetch_add_explicit(&hdr->active_consumers, 1, memory_order_relaxed);

    *out_ring = ring;
    return RBIPC_OK;
}

int rbipc_detach(rbipc_ring_t *ring) {
    if (!ring) {
        return RBIPC_ERR_INVAL;
    }

    if (ring->hdr) {
        if (ring->is_creator) {
            atomic_fetch_sub_explicit(&ring->hdr->active_producers, 1, memory_order_relaxed);
        } else {
            atomic_fetch_sub_explicit(&ring->hdr->active_consumers, 1, memory_order_relaxed);
        }
    }

    if (ring->data_map && ring->data_size > 0) {
        munmap(ring->data_map, 2 * ring->data_size);
        ring->data_map = NULL;
    }

    if (ring->ctrl_map && ring->ctrl_map_size > 0) {
        munmap(ring->ctrl_map, ring->ctrl_map_size);
        ring->ctrl_map = NULL;
    }

    if (ring->fd >= 0) {
        close(ring->fd);
        ring->fd = -1;
    }

    free(ring);
    return RBIPC_OK;
}

int rbipc_destroy(const char *name) {
    if (!name) {
        return RBIPC_ERR_INVAL;
    }
    if (shm_unlink(name) != 0) {
        return RBIPC_ERR_SYS;
    }
    return RBIPC_OK;
}

int rbipc_signal_shutdown(rbipc_ring_t *ring) {
    if (!ring || !ring->hdr) {
        return RBIPC_ERR_INVAL;
    }
    atomic_store_explicit(&ring->hdr->shutdown_flag, 1, memory_order_release);
    atomic_fetch_add_explicit(&ring->hdr->futex_seq, 1, memory_order_release);
    futex_wake(&ring->hdr->futex_seq, INT32_MAX);
    return RBIPC_OK;
}

int rbipc_get_fd(const rbipc_ring_t *ring) {
    return ring ? ring->fd : -1;
}

/* ============================================================================
 * Lock-Free Core: Reserve, Commit, Acquire, Release
 * ============================================================================ */

int rbipc_reserve_write(rbipc_ring_t *ring, uint32_t len, void **out_buf, uint32_t *ticket) {
    if (!ring || !out_buf || !ticket || len > ring->hdr->slot_size) {
        return RBIPC_ERR_INVAL;
    }

    rbipc_shm_header_t *hdr = ring->hdr;
    const uint32_t mask = hdr->capacity_mask;
    uint32_t t = atomic_load_explicit(&hdr->write_ticket, memory_order_relaxed);
    uint32_t spin_count = 0;

    struct timespec futex_timeout = { .tv_sec = 0, .tv_nsec = 50000000 }; /* 50ms */

    for (;;) {
        if (atomic_load_explicit(&hdr->shutdown_flag, memory_order_relaxed)) {
            return RBIPC_ERR_SHUTDOWN;
        }

        rbipc_slot_t *slot = &ring->slots[t & mask];
        uint32_t seq = atomic_load_explicit(&slot->sequence, memory_order_acquire);
        int32_t diff = (int32_t)(seq - t);

        if (diff == 0) {
            /* Slot is vacant and matches write ticket turn. Attempt to claim ticket. */
            if (atomic_compare_exchange_weak_explicit(&hdr->write_ticket, &t, t + 1,
                                                      memory_order_relaxed, memory_order_relaxed)) {
                /* Exclusively reserved slot 't' */
                atomic_store_explicit(&slot->producer_pid, (uint32_t)getpid(), memory_order_relaxed);
                atomic_store_explicit(&slot->state, RBIPC_SLOT_RESERVED, memory_order_release);

                /* Calculate zero-copy virtual pointer in double-mapped region */
                *out_buf = (char *)ring->data_map + ((size_t)(t & mask) * hdr->slot_size);
                *ticket = t;
                return RBIPC_OK;
            }
            /* CAS contention: t was updated with latest write_ticket, retry immediately */
            spin_count = 0;
        } else if (diff < 0) {
            /* Buffer is FULL: Slot t has not yet been processed and released by consumer.
             * Apply 3-Tier Hybrid Backoff:
             *   Tier 1: Spin loop with cpu_pause()
             *   Tier 2: Voluntary context yield via sched_yield()
             *   Tier 3: OS-level kernel wait via sys_futex
             */
            spin_count++;
            if (spin_count < SPIN_TIER_PAUSE) {
                cpu_pause();
            } else if (spin_count < SPIN_TIER_YIELD) {
                sched_yield();
            } else {
                uint32_t fseq = atomic_load_explicit(&hdr->futex_seq, memory_order_relaxed);
                futex_wait(&hdr->futex_seq, fseq, &futex_timeout);
                spin_count = 0;
            }
            t = atomic_load_explicit(&hdr->write_ticket, memory_order_relaxed);
        } else {
            /* Another producer already claimed ticket t, refresh t */
            t = atomic_load_explicit(&hdr->write_ticket, memory_order_relaxed);
            spin_count = 0;
        }
    }
}

int rbipc_commit_write(rbipc_ring_t *ring, uint32_t ticket, uint32_t written_len) {
    if (!ring || written_len > ring->hdr->slot_size) {
        return RBIPC_ERR_INVAL;
    }

    rbipc_shm_header_t *hdr = ring->hdr;
    rbipc_slot_t *slot = &ring->slots[ticket & hdr->capacity_mask];

    /* Record payload length and mark state */
    atomic_store_explicit(&slot->len, written_len, memory_order_relaxed);
    atomic_store_explicit(&slot->state, RBIPC_SLOT_COMMITTED, memory_order_release);

    /* Advance sequence to ticket + 1 with release semantics to publish payload to consumers */
    atomic_store_explicit(&slot->sequence, ticket + 1, memory_order_release);

    /* Wake consumers waiting on futex */
    atomic_fetch_add_explicit(&hdr->futex_seq, 1, memory_order_release);
    futex_wake(&hdr->futex_seq, 1);

    return RBIPC_OK;
}

int rbipc_read_acquire(rbipc_ring_t *ring, const void **out_buf, uint32_t *out_len, uint32_t *ticket) {
    if (!ring || !out_buf || !out_len || !ticket) {
        return RBIPC_ERR_INVAL;
    }

    rbipc_shm_header_t *hdr = ring->hdr;
    const uint32_t mask = hdr->capacity_mask;
    uint32_t t = atomic_load_explicit(&hdr->read_ticket, memory_order_relaxed);
    uint32_t backoff_count = 0;
    uint32_t reserved_wait_count = 0;

    struct timespec futex_timeout = { .tv_sec = 0, .tv_nsec = 20000000 }; /* 20ms */

    for (;;) {
        rbipc_slot_t *slot = &ring->slots[t & mask];
        uint32_t seq = atomic_load_explicit(&slot->sequence, memory_order_acquire);
        int32_t diff = (int32_t)(seq - (t + 1));

        if (diff == 0) {
            /* Slot t is committed or poisoned and ready for consumption */
            if (atomic_compare_exchange_weak_explicit(&hdr->read_ticket, &t, t + 1,
                                                      memory_order_relaxed, memory_order_relaxed)) {
                uint32_t state = atomic_load_explicit(&slot->state, memory_order_acquire);
                if (state == RBIPC_SLOT_COMMITTED) {
                    *out_buf = (const char *)ring->data_map + ((size_t)(t & mask) * hdr->slot_size);
                    *out_len = atomic_load_explicit(&slot->len, memory_order_acquire);
                    *ticket = t;
                    return RBIPC_OK;
                } else if (state == RBIPC_SLOT_POISONED) {
                    /* Producer crashed; caller is notified to skip/handle poisoned slot */
                    *out_buf = NULL;
                    *out_len = 0;
                    *ticket = t;
                    return RBIPC_ERR_POISONED;
                }
            }
            backoff_count = 0;
            reserved_wait_count = 0;
        } else if (diff < 0) {
            /* Slot has not yet been committed. Check if holding producer crashed! */
            uint32_t state = atomic_load_explicit(&slot->state, memory_order_acquire);
            if (state == RBIPC_SLOT_RESERVED) {
                reserved_wait_count++;
                if (reserved_wait_count >= 500) {
                    uint32_t pid = atomic_load_explicit(&slot->producer_pid, memory_order_relaxed);
                    if (pid > 0 && kill((pid_t)pid, 0) == -1 && errno == ESRCH) {
                        /* Producer crashed while holding reservation! Transition to poisoned */
                        uint32_t expected_state = RBIPC_SLOT_RESERVED;
                        if (atomic_compare_exchange_strong_explicit(&slot->state, &expected_state,
                                                                    RBIPC_SLOT_POISONED,
                                                                    memory_order_release,
                                                                    memory_order_relaxed)) {
                            atomic_store_explicit(&slot->sequence, t + 1, memory_order_release);
                            atomic_fetch_add_explicit(&hdr->futex_seq, 1, memory_order_release);
                            futex_wake(&hdr->futex_seq, 1);
                        }
                    }
                    reserved_wait_count = 0;
                }
            } else {
                reserved_wait_count = 0;
            }

            if (atomic_load_explicit(&hdr->shutdown_flag, memory_order_relaxed)) {
                /* If shutdown signaled and no committed data remains, exit */
                uint32_t cur_write = atomic_load_explicit(&hdr->write_ticket, memory_order_relaxed);
                if (t >= cur_write) {
                    return RBIPC_ERR_SHUTDOWN;
                }
            }

            /* Hybrid Wait Strategy */
            if (backoff_count < SPIN_TIER_PAUSE) {
                cpu_pause();
                backoff_count++;
            } else if (backoff_count < SPIN_TIER_YIELD) {
                sched_yield();
                backoff_count++;
            } else {
                uint32_t fseq = atomic_load_explicit(&hdr->futex_seq, memory_order_relaxed);
                futex_wait(&hdr->futex_seq, fseq, &futex_timeout);
                backoff_count = 0;
            }
            t = atomic_load_explicit(&hdr->read_ticket, memory_order_relaxed);
        } else {
            /* Another consumer claimed ticket t */
            t = atomic_load_explicit(&hdr->read_ticket, memory_order_relaxed);
            backoff_count = 0;
            reserved_wait_count = 0;
        }
    }
}

int rbipc_read_release(rbipc_ring_t *ring, uint32_t ticket) {
    if (!ring) {
        return RBIPC_ERR_INVAL;
    }

    rbipc_shm_header_t *hdr = ring->hdr;
    rbipc_slot_t *slot = &ring->slots[ticket & hdr->capacity_mask];

    /* Mark slot vacant */
    atomic_store_explicit(&slot->state, RBIPC_SLOT_EMPTY, memory_order_release);

    /* Advance slot sequence by capacity to allow next cycle turn */
    atomic_store_explicit(&slot->sequence, ticket + hdr->capacity, memory_order_release);

    /* Wake producers waiting on futex */
    atomic_fetch_add_explicit(&hdr->futex_seq, 1, memory_order_release);
    futex_wake(&hdr->futex_seq, 1);

    return RBIPC_OK;
}
