/**
 * @file rbipc_vmem.c
 * @brief Implementation of virtual memory management and double-mapped circular ring mirror.
 * @details Implements kernel virtual memory mappings using POSIX @c mmap and @c munmap.
 *          Provides the mechanics for reserving anonymous memory space and binding
 *          the physical shared memory file twice consecutively with @c MAP_FIXED.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "rbipc.h"
#include "rbipc_vmem.h"
#include "rbipc_attr.h"
#include "rbipc_predicate.h"

#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>

/**
 * @brief Maps the control header and slot descriptor metadata region.
 * @details Invokes @c mmap with flags @c MAP_SHARED and protections @c PROT_READ | @c PROT_WRITE.
 *
 * @param[in]  fd       Shared memory file descriptor.
 * @param[in]  size     Size in bytes of control region (page aligned).
 * @param[out] out_map  Pointer to receive the mapped address.
 *
 * @return Status code indicating the outcome of the mapping operation.
 * @retval RBIPC_OK        Control region successfully mapped.
 * @retval RBIPC_ERR_INVAL Invalid descriptor, zero size, or null output pointer.
 * @retval RBIPC_ERR_SYS   Kernel @c mmap failed.
 */
int rbipc_vmem_map_ctrl(int fd, size_t size, void ** RBIPC_RESTRICT out_map) {
    if (RBIPC_UNLIKELY(!rbipc_is_valid_fd(fd) || size == 0 || !out_map)) {
        return RBIPC_ERR_INVAL;
    }

    void *ctrl = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ctrl == MAP_FAILED) {
        return RBIPC_ERR_SYS;
    }

    *out_map = ctrl;
    return RBIPC_OK;
}

/**
 * @brief Unmaps the control header and slot metadata virtual memory region.
 * @details Invokes @c munmap on the previously allocated control address range.
 *
 * @param[in] map  Base virtual address of the mapped control region.
 * @param[in] size Size in bytes of the mapped control region.
 */
RBIPC_LEAF
void rbipc_vmem_unmap_ctrl(void *map, size_t size) {
    if (map && size > 0) {
        munmap(map, size);
    }
}

/**
 * @brief Reserves a contiguous span of uncommitted virtual address space.
 * @details Obtains a reservation of \(2 \times \text{data\_size}\) bytes with @c PROT_NONE
 *          and @c MAP_PRIVATE | @c MAP_ANONYMOUS to guarantee that no other allocation
 *          can be placed in the target virtual address range.
 *
 * @param[in]  total_size Size in bytes of the address space span to reserve (\(2 \times \text{data\_size}\)).
 * @param[out] out_anon   Pointer to receive the allocated base address.
 *
 * @return Status code indicating the result of the virtual address reservation.
 * @retval RBIPC_OK         Address space reservation succeeded.
 * @retval RBIPC_ERR_NOMEM  Kernel failed to allocate virtual address space.
 */
RBIPC_INLINE int rbipc_vmem_reserve_address_space(size_t total_size, void ** RBIPC_RESTRICT out_anon) {
    void *anon = mmap(NULL, total_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (anon == MAP_FAILED) {
        return RBIPC_ERR_NOMEM;
    }
    *out_anon = anon;
    return RBIPC_OK;
}

/**
 * @brief Binds both halves of the reserved virtual address span to the shared memory data region.
 * @details Overwrites the reserved address space with two consecutive shared mappings
 *          utilizing @c MAP_FIXED. Both mappings reference the exact same file descriptor
 *          and file byte offset.
 *
 * @param[in] fd        Shared memory file descriptor.
 * @param[in] offset    Byte offset within the shared memory file where the data buffer begins.
 * @param[in] data_size Size in bytes of a single data buffer.
 * @param[in] anon      Base virtual address of the reserved address space span.
 *
 * @return Status code indicating the result of the binding operation.
 * @retval RBIPC_OK      Both mirror halves successfully bound to the shared memory backing store.
 * @retval RBIPC_ERR_SYS Kernel @c mmap failed for either half.
 */
RBIPC_INLINE int rbipc_vmem_bind_mirror_halves(int fd, size_t offset, size_t data_size, void *anon) {
    /* Map first circular buffer half */
    void *m1 = mmap(anon, data_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, (off_t)offset);
    if (m1 == MAP_FAILED) {
        return RBIPC_ERR_SYS;
    }

    /* Map second circular buffer half to the exact same shared memory offset */
    void *m2 = mmap((char *)anon + data_size, data_size, PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_FIXED, fd, (off_t)offset);
    if (m2 == MAP_FAILED) {
        return RBIPC_ERR_SYS;
    }

    return RBIPC_OK;
}

/**
 * @brief Constructs a double-mapped contiguous virtual address mirror for the circular data buffer.
 * @details Atomically orchestrates the reservation of \(2 \times \text{data\_size}\) virtual address space
 *          followed by consecutive @c MAP_FIXED bindings of both mirror halves.
 *
 * @param[in]  fd           Shared memory file descriptor.
 * @param[in]  offset       Byte offset within the shared memory file where data begins.
 * @param[in]  data_size    Size in bytes of a single data buffer.
 * @param[out] out_data_map Pointer to receive the base address of the double-mapped mirror.
 *
 * @return Status code indicating the outcome of the mirror creation.
 * @retval RBIPC_OK        Double-mapped mirror established successfully.
 * @retval RBIPC_ERR_INVAL Invalid descriptor, zero size, or null output pointer.
 * @retval RBIPC_ERR_NOMEM Virtual address space allocation failure.
 * @retval RBIPC_ERR_SYS   Kernel mapping failure during mirror binding.
 */
int rbipc_vmem_map_double(int fd, size_t offset, size_t data_size, void ** RBIPC_RESTRICT out_data_map) {
    if (RBIPC_UNLIKELY(!rbipc_is_valid_fd(fd) || data_size == 0 || !out_data_map)) {
        return RBIPC_ERR_INVAL;
    }

    void *anon = NULL;
    int rc = rbipc_vmem_reserve_address_space(2 * data_size, &anon);
    if (RBIPC_UNLIKELY(rc != RBIPC_OK)) {
        return rc;
    }

    rc = rbipc_vmem_bind_mirror_halves(fd, offset, data_size, anon);
    if (RBIPC_UNLIKELY(rc != RBIPC_OK)) {
        munmap(anon, 2 * data_size);
        return rc;
    }

    *out_data_map = anon;
    return RBIPC_OK;
}

/**
 * @brief Unmaps the double-mapped virtual buffer mirror.
 * @details Unmaps the entire contiguous \(2 \times \text{data\_size}\) virtual address range.
 *
 * @param[in] data_map  Contiguous double-mapped base pointer.
 * @param[in] data_size Size in bytes of a single circular buffer.
 */
RBIPC_LEAF
void rbipc_vmem_unmap_double(void *data_map, size_t data_size) {
    if (data_map && data_size > 0) {
        munmap(data_map, 2 * data_size);
    }
}

