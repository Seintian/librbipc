/**
 * @file rbipc_shm.c
 * @brief Implementation of shared memory descriptor management and layout
 * geometry.
 *
 * @details Implements overflow-safe geometric partition sizing, file descriptor
 * creation via shm_open or memfd_create, kernel file sealing, and descriptor
 * teardown.
 *
 * @author Seintian
 * @date 2026
 * @copyright Apache License 2.0
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "rbipc_shm.h"
#include "rbipc.h"
#include "rbipc_attr.h"
#include "rbipc_math.h"
#include "rbipc_predicate.h"

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef F_LINUX_SPECIFIC_BASE
#define F_LINUX_SPECIFIC_BASE 1024
#endif
#ifndef F_ADD_SEALS
#define F_ADD_SEALS (F_LINUX_SPECIFIC_BASE + 9)
#define F_GET_SEALS (F_LINUX_SPECIFIC_BASE + 10)
#define F_SEAL_SEAL 0x0001
#define F_SEAL_SHRINK 0x0002
#define F_SEAL_GROW 0x0004
#endif

#ifndef MFD_HUGETLB
#define MFD_HUGETLB 0x0004U
#endif
#ifndef MFD_HUGE_2MB
#define MFD_HUGE_2MB (21U << 26U)
#endif
#ifndef MAP_HUGETLB
#define MAP_HUGETLB 0x40000U
#endif
#ifndef MAP_HUGE_2MB
#define MAP_HUGE_2MB (21U << 26U)
#endif

/**
 * @brief Compute cache-line aligned slot byte capacity with overflow checking.
 *
 * @param[in]  slot_size   Raw unaligned payload size.
 * @param[out] out_aligned Pointer receiving aligned slot size.
 *
 * @return @ref RBIPC_OK on success, @ref RBIPC_ERR_OVERFLOW if value overflows
 * 32 bits.
 */
RBIPC_INLINE int
rbipc_shm_calc_aligned_slot_size(uint32_t slot_size,
                                 size_t *RBIPC_RESTRICT out_aligned) {
  if (slot_size > UINT32_MAX - RBIPC_CACHE_LINE) {
    return RBIPC_ERR_OVERFLOW;
  }
  size_t aligned = rbipc_align_up(slot_size, RBIPC_CACHE_LINE);
  if (aligned == 0 || aligned > UINT32_MAX) {
    return RBIPC_ERR_OVERFLOW;
  }
  *out_aligned = aligned;
  return RBIPC_OK;
}

/**
 * @brief Compute page-aligned single circular data buffer size with
 * multiplication overflow check.
 *
 * @param[in]  capacity          Slot count.
 * @param[in]  aligned_slot_size Aligned slot byte size.
 * @param[in]  page_size         System page size.
 * @param[out] out_data_size     Pointer receiving page-aligned data buffer byte
 * size.
 *
 * @return @ref RBIPC_OK on success, @ref RBIPC_ERR_OVERFLOW on overflow.
 */
RBIPC_INLINE int
rbipc_shm_calc_data_geometry(uint32_t capacity, size_t aligned_slot_size,
                             size_t page_size,
                             size_t *RBIPC_RESTRICT out_data_size) {
  if (aligned_slot_size > SIZE_MAX / capacity) {
    return RBIPC_ERR_OVERFLOW;
  }
  size_t raw_data_size = (size_t)capacity * aligned_slot_size;
  size_t align = page_size;
  if (raw_data_size >= RBIPC_HUGE_PAGE_2MB && RBIPC_HUGE_PAGE_2MB > align) {
    align = RBIPC_HUGE_PAGE_2MB;
  }
  size_t data_size = rbipc_align_up(raw_data_size, align);
  if (data_size < raw_data_size) {
    return RBIPC_ERR_OVERFLOW;
  }
  *out_data_size = data_size;
  return RBIPC_OK;
}

/**
 * @brief Compute metadata structure size and page-aligned payload data offset.
 *
 * @param[in]  capacity        Slot count.
 * @param[in]  page_size       System page size.
 * @param[out] out_data_offset Pointer receiving page-aligned offset to ring
 * data buffer.
 *
 * @return @ref RBIPC_OK on success, @ref RBIPC_ERR_OVERFLOW on overflow.
 */
RBIPC_INLINE int
rbipc_shm_calc_metadata_geometry(uint32_t capacity, size_t page_size,
                                 size_t *RBIPC_RESTRICT out_data_offset) {
  if (sizeof(rbipc_slot_t) >
      (SIZE_MAX - sizeof(rbipc_shm_header_t)) / capacity) {
    return RBIPC_ERR_OVERFLOW;
  }
  size_t header_and_slots =
      sizeof(rbipc_shm_header_t) + ((size_t)capacity * sizeof(rbipc_slot_t));
  size_t data_offset = rbipc_align_up(header_and_slots, page_size);
  if (data_offset < header_and_slots) {
    return RBIPC_ERR_OVERFLOW;
  }
  *out_data_offset = data_offset;
  return RBIPC_OK;
}

/**
 * @brief Calculate shared memory partition offsets and layout geometry.
 */
RBIPC_LEAF
int rbipc_shm_calc_layout(uint32_t capacity, uint32_t slot_size,
                          size_t page_size,
                          rbipc_layout_t *RBIPC_RESTRICT layout) {
  if (RBIPC_UNLIKELY(!layout || !rbipc_is_power_of_two(capacity) ||
                     capacity < 2 || slot_size == 0 || page_size == 0)) {
    return RBIPC_ERR_INVAL;
  }

  size_t aligned_slot_size = 0;
  int rc = rbipc_shm_calc_aligned_slot_size(slot_size, &aligned_slot_size);
  if (RBIPC_UNLIKELY(rc != RBIPC_OK))
    return rc;

  size_t data_size = 0;
  rc = rbipc_shm_calc_data_geometry(capacity, aligned_slot_size, page_size,
                                    &data_size);
  if (RBIPC_UNLIKELY(rc != RBIPC_OK))
    return rc;

  size_t offset_align = page_size;
  if (data_size >= RBIPC_HUGE_PAGE_2MB && RBIPC_HUGE_PAGE_2MB > offset_align) {
    offset_align = RBIPC_HUGE_PAGE_2MB;
  }

  size_t data_offset = 0;
  rc = rbipc_shm_calc_metadata_geometry(capacity, offset_align, &data_offset);
  if (RBIPC_UNLIKELY(rc != RBIPC_OK))
    return rc;

  if (RBIPC_UNLIKELY(data_size > SIZE_MAX - data_offset)) {
    return RBIPC_ERR_OVERFLOW;
  }
  size_t total_shm_size = data_offset + data_size;

  layout->capacity = capacity;
  layout->aligned_slot_size = (uint32_t)aligned_slot_size;
  layout->data_offset = data_offset;
  layout->data_size = data_size;
  layout->total_shm_size = total_shm_size;

  return RBIPC_OK;
}

static _Atomic int s_hugetlb_available = -1;

RBIPC_LEAF
bool rbipc_shm_hugetlb_available(void) {
  int cached = atomic_load_explicit(&s_hugetlb_available, memory_order_relaxed);
  if (RBIPC_LIKELY(cached >= 0)) {
    return cached == 1;
  }

  int fd = memfd_create("rbipc_huge_probe",
                        MFD_CLOEXEC | MFD_HUGETLB | MFD_HUGE_2MB);
  if (fd < 0) {
    atomic_store_explicit(&s_hugetlb_available, 0, memory_order_relaxed);
    return false;
  }

  bool ok = false;
  if (ftruncate(fd, (off_t)RBIPC_HUGE_PAGE_2MB) == 0) {
    void *p = mmap(NULL, RBIPC_HUGE_PAGE_2MB, PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, 0);
    if (p != MAP_FAILED) {
      ok = true;
      munmap(p, RBIPC_HUGE_PAGE_2MB);
    }
  }
  close(fd);

  atomic_store_explicit(&s_hugetlb_available, ok ? 1 : 0, memory_order_relaxed);
  return ok;
}

/**
 * @brief Atomic helper: Create an anonymous memory file descriptor via Linux
 * memfd_create.
 */
RBIPC_INLINE int rbipc_shm_create_anonymous(const char *name, mode_t mode,
                                            uint32_t flags,
                                            int *RBIPC_RESTRICT out_fd) {
  const char *desc_name = RBIPC_ANON_MEMFD_NAME;
  if (name && name[0] != '\0') {
    desc_name = (name[0] == '/') ? &name[1] : name;
  }

  int fd = -1;
  if ((flags & RBIPC_FLAG_HUGETLB) && rbipc_shm_hugetlb_available()) {
    fd = memfd_create(desc_name,
                      MFD_CLOEXEC | MFD_ALLOW_SEALING | MFD_HUGETLB | MFD_HUGE_2MB);
  }
  if (fd < 0) {
    fd = memfd_create(desc_name, MFD_CLOEXEC | MFD_ALLOW_SEALING);
  }
  if (fd < 0) {
    return RBIPC_ERR_SYS;
  }
  mode_t actual_mode =
      (mode == 0) ? RBIPC_DEFAULT_SHM_MODE : (mode & (mode_t)07777U);
  if (fchmod(fd, actual_mode) != 0) {
    int err = errno;
    close(fd);
    errno = err;
    return RBIPC_ERR_SYS;
  }
  *out_fd = fd;
  return RBIPC_OK;
}

/**
 * @brief Atomic helper: Create and exclusively open a POSIX shared memory
 * object.
 */
RBIPC_INLINE int rbipc_shm_create_named(const char *RBIPC_RESTRICT name,
                                        uint32_t flags, mode_t mode,
                                        int *RBIPC_RESTRICT out_fd) {
  if (RBIPC_UNLIKELY(!rbipc_is_valid_shm_name(name))) {
    return RBIPC_ERR_INVAL;
  }
  if (flags & RBIPC_CREATE_OVERWRITE) {
    shm_unlink(name);
  }
  mode_t actual_mode =
      (mode == 0) ? RBIPC_DEFAULT_SHM_MODE : (mode & (mode_t)07777U);
  int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, actual_mode);
  if (fd < 0) {
    if (errno == EEXIST) {
      return RBIPC_ERR_EXISTS;
    }
    return RBIPC_ERR_SYS;
  }
  /* Explicitly enforce permissions via fchmod to bypass restrictive process
   * umask */
  if (fchmod(fd, actual_mode) != 0) {
    int err = errno;
    close(fd);
    shm_unlink(name);
    errno = err;
    return RBIPC_ERR_SYS;
  }
  *out_fd = fd;
  return RBIPC_OK;
}

/**
 * @brief Atomic helper: Truncate file descriptor to desired byte capacity.
 */
RBIPC_INLINE int rbipc_shm_truncate_file(int fd, size_t total_size) {
  if (ftruncate(fd, (off_t)total_size) != 0) {
    return RBIPC_ERR_SYS;
  }
  return RBIPC_OK;
}

/**
 * @brief Create and truncate a shared memory file descriptor with mode and
 * flags.
 */
int rbipc_shm_create_ex(const char *RBIPC_RESTRICT name, size_t total_size,
                        uint32_t flags, mode_t mode,
                        int *RBIPC_RESTRICT out_fd) {
  if (RBIPC_UNLIKELY(!out_fd || total_size == 0)) {
    return RBIPC_ERR_INVAL;
  }
  uint32_t known_flags =
      RBIPC_CREATE_OVERWRITE | RBIPC_SHM_ANON | RBIPC_SHM_NAMED | RBIPC_FLAG_HUGETLB;
  if (RBIPC_UNLIKELY((flags & ~known_flags) != 0)) {
    return RBIPC_ERR_INVAL;
  }
  if (RBIPC_UNLIKELY((flags & RBIPC_SHM_ANON) && (flags & RBIPC_SHM_NAMED))) {
    return RBIPC_ERR_INVAL;
  }
  if (RBIPC_UNLIKELY((flags & RBIPC_FLAG_HUGETLB) && (flags & RBIPC_SHM_NAMED))) {
    return RBIPC_ERR_INVAL;
  }

  bool is_named = false;
  if (flags & RBIPC_SHM_NAMED) {
    if (RBIPC_UNLIKELY(!name)) {
      return RBIPC_ERR_INVAL;
    }
    is_named = true;
  } else if (flags & RBIPC_SHM_ANON) {
    is_named = false;
  } else {
    /* Anonymous is the default if user does not emit an explicit choice (name == NULL) */
    is_named = (name != NULL);
  }

  int fd = -1;
  int rc = is_named ? rbipc_shm_create_named(name, flags, mode, &fd)
                    : rbipc_shm_create_anonymous(name, mode, flags, &fd);
  if (RBIPC_UNLIKELY(rc != RBIPC_OK)) {
    return rc;
  }

  size_t trunc_size = total_size;
  if ((flags & RBIPC_FLAG_HUGETLB) && rbipc_shm_hugetlb_available()) {
    trunc_size = rbipc_align_up(total_size, RBIPC_HUGE_PAGE_2MB);
  }

  rc = rbipc_shm_truncate_file(fd, trunc_size);
  if (RBIPC_UNLIKELY(rc != RBIPC_OK)) {
    /* If truncation failed on hugetlb fd, fallback gracefully to standard memfd */
    if ((flags & RBIPC_FLAG_HUGETLB) && !is_named) {
      close(fd);
      rc = rbipc_shm_create_anonymous(name, mode, 0, &fd);
      if (RBIPC_UNLIKELY(rc != RBIPC_OK)) {
        return rc;
      }
      rc = rbipc_shm_truncate_file(fd, total_size);
      if (RBIPC_UNLIKELY(rc != RBIPC_OK)) {
        int err = errno;
        close(fd);
        errno = err;
        return rc;
      }
      *out_fd = fd;
      return RBIPC_OK;
    }
    int err = errno;
    close(fd);
    if (is_named && name)
      shm_unlink(name);
    errno = err;
    return rc;
  }

  *out_fd = fd;
  return RBIPC_OK;
}

/**
 * @brief Open an existing POSIX shared memory object.
 */
int rbipc_shm_open(const char *RBIPC_RESTRICT name,
                   int *RBIPC_RESTRICT out_fd) {
  if (RBIPC_UNLIKELY(!out_fd || !rbipc_is_valid_shm_name(name))) {
    return RBIPC_ERR_INVAL;
  }

  int fd = shm_open(name, O_RDWR, 0);

  if (fd < 0) {
    return RBIPC_ERR_SYS;
  }

  *out_fd = fd;
  return RBIPC_OK;
}

/**
 * @brief Apply file sealing to shared memory file descriptor.
 */
RBIPC_LEAF
int rbipc_shm_seal(int fd) {
  if (RBIPC_UNLIKELY(!rbipc_is_valid_fd(fd))) {
    return RBIPC_ERR_INVAL;
  }
#ifdef F_ADD_SEALS
  if (fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW) < 0) {
    if (errno == EINVAL || errno == EPERM || errno == ENOSYS ||
        errno == EOPNOTSUPP) {
      return RBIPC_ERR_NOTSUP;
    }
    return RBIPC_ERR_SYS;
  }
  return RBIPC_OK;
#else
  return RBIPC_ERR_NOTSUP;
#endif
}

/**
 * @brief Unlink a POSIX shared memory object by name.
 */
RBIPC_LEAF
int rbipc_shm_unlink(const char *name) {
  if (RBIPC_UNLIKELY(!rbipc_is_valid_shm_name(name))) {
    return RBIPC_ERR_INVAL;
  }
  if (shm_unlink(name) != 0) {
    return RBIPC_ERR_SYS;
  }
  return RBIPC_OK;
}

/**
 * @brief Close an open shared memory file descriptor.
 */
RBIPC_LEAF
void rbipc_shm_close(int fd) {
  if (rbipc_is_valid_fd(fd)) {
    close(fd);
  }
}
