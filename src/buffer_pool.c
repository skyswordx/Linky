#define _GNU_SOURCE
#include "buffer_pool.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

#if defined(__has_include)
#if __has_include(<linux/dma-buf.h>)
#include <linux/dma-buf.h>
#endif
#if __has_include(<linux/dma-heap.h>)
#include <linux/dma-heap.h>
#endif
#endif

#ifndef DMA_BUF_SYNC_READ
#define DMA_BUF_SYNC_READ (1ULL << 0)
#define DMA_BUF_SYNC_WRITE (2ULL << 0)
#define DMA_BUF_SYNC_RW (DMA_BUF_SYNC_READ | DMA_BUF_SYNC_WRITE)
#define DMA_BUF_SYNC_START (0ULL << 2)
#define DMA_BUF_SYNC_END (1ULL << 2)
struct dma_buf_sync {
    uint64_t flags;
};
#define DMA_BUF_BASE 'b'
#define DMA_BUF_IOCTL_SYNC _IOW(DMA_BUF_BASE, 0, struct dma_buf_sync)
#endif

#ifndef DMA_HEAP_IOCTL_ALLOC
struct dma_heap_allocation_data {
    uint64_t len;
    uint32_t fd;
    uint32_t fd_flags;
    uint64_t heap_flags;
};
#define DMA_HEAP_IOC_MAGIC 'H'
#define DMA_HEAP_IOCTL_ALLOC _IOWR(DMA_HEAP_IOC_MAGIC, 0x0, struct dma_heap_allocation_data)
#endif

static int linky_memfd_create(const char *name, unsigned int flags)
{
#ifdef SYS_memfd_create
    return (int)syscall(SYS_memfd_create, name, flags);
#else
    (void)name;
    (void)flags;
    errno = ENOSYS;
    return -1;
#endif
}

size_t linky_pool_total_size(uint32_t count, size_t frame_size)
{
    if (count == 0 || frame_size == 0 || frame_size > SIZE_MAX / count) {
        return 0;
    }
    return (size_t)count * frame_size;
}

int linky_pool_alloc_heap(linky_buffer_pool_t *pool, uint32_t count, size_t frame_size)
{
    size_t total = linky_pool_total_size(count, frame_size);
    int ret;

    if (pool == NULL || total == 0) {
        return -EINVAL;
    }

    /*
     * Experiment 1: ordinary userspace memory.
     *
     * This validates the buffer-pool idea only. The fd field stays -1 because
     * malloc/posix_memalign memory is not an fd-backed shared object. A real
     * device cannot import this pointer directly; a driver would still need to
     * pin/map it before DMA.
     */
    memset(pool, 0, sizeof(*pool));
    ret = posix_memalign(&pool->base, LINKY_CACHELINE, total);
    if (ret != 0) {
        return -ret;
    }
    memset(pool->base, 0, total);
    pool->frame_size = frame_size;
    pool->count = count;
    pool->fd = -1;
    pool->owns_mapping = 0;
    pool->owns_fd = 0;
    return 0;
}

int linky_pool_alloc_memfd(linky_buffer_pool_t *pool, uint32_t count, size_t frame_size)
{
    size_t total = linky_pool_total_size(count, frame_size);
    void *mapping;
    int fd;

    if (pool == NULL || total == 0) {
        return -EINVAL;
    }

    /*
     * Experiment 2: fd-backed shared memory.
     *
     * memfd_create() returns an fd for an anonymous kernel file. mmap() gives
     * the process a CPU virtual address. This is useful for learning the
     * distinction between "fd as handle" and "pointer as CPU mapping".
     *
     * memfd is not DMA-BUF. It teaches fd + mmap + descriptor passing, but a
     * GPU/V4L2/NPU driver cannot automatically import this fd as a dma-buf.
     */
    memset(pool, 0, sizeof(*pool));
    fd = linky_memfd_create("linky-memfd-pool", MFD_CLOEXEC);
    if (fd < 0) {
        return -errno;
    }

    if (ftruncate(fd, (off_t)total) != 0) {
        int saved = errno;
        close(fd);
        return -saved;
    }

    mapping = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED) {
        int saved = errno;
        close(fd);
        return -saved;
    }

    memset(mapping, 0, total);
    pool->base = mapping;
    pool->frame_size = frame_size;
    pool->count = count;
    pool->fd = fd;
    pool->owns_mapping = 1;
    pool->owns_fd = 1;
    return 0;
}

int linky_pool_alloc_dmabuf(linky_buffer_pool_t *pool,
                            uint32_t count,
                            size_t frame_size,
                            const char *heap_path)
{
    size_t total = linky_pool_total_size(count, frame_size);
    struct dma_heap_allocation_data data;
    void *mapping;
    int heap_fd;
    int ret;

    if (pool == NULL || total == 0 || heap_path == NULL) {
        return -EINVAL;
    }

    /*
     * Experiment 3: DMA-BUF heap.
     *
     * /dev/dma_heap/system is an allocator endpoint. The allocation ioctl
     * returns a new dma-buf fd. That fd names a kernel dma_buf object and can
     * potentially be imported by other drivers on systems that support it.
     *
     * mmap() below creates a CPU virtual mapping of the same shared buffer.
     * CPU mapping is convenient for this lab, but real device pipelines try to
     * keep the CPU away from large frame data because CPU access may require
     * cache maintenance and synchronization.
     */
    memset(pool, 0, sizeof(*pool));
    heap_fd = open(heap_path, O_RDONLY | O_CLOEXEC);
    if (heap_fd < 0) {
        return -errno;
    }

    memset(&data, 0, sizeof(data));
    data.len = total;
    data.fd_flags = O_CLOEXEC | O_RDWR;
    data.heap_flags = 0;

    ret = ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &data);
    close(heap_fd);
    if (ret != 0) {
        return -errno;
    }

    mapping = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, data.fd, 0);
    if (mapping == MAP_FAILED) {
        int saved = errno;
        close((int)data.fd);
        return -saved;
    }

    memset(mapping, 0, total);
    pool->base = mapping;
    pool->frame_size = frame_size;
    pool->count = count;
    pool->fd = (int)data.fd;
    pool->owns_mapping = 1;
    pool->owns_fd = 1;
    return 0;
}

static uint64_t sync_flags(linky_cpu_access_t access, uint64_t phase)
{
    uint64_t direction = 0;

    if ((access & LINKY_CPU_ACCESS_READ) != 0) {
        direction |= DMA_BUF_SYNC_READ;
    }
    if ((access & LINKY_CPU_ACCESS_WRITE) != 0) {
        direction |= DMA_BUF_SYNC_WRITE;
    }

    return direction | phase;
}

int linky_pool_begin_cpu_access(const linky_buffer_pool_t *pool, linky_cpu_access_t access)
{
    struct dma_buf_sync sync;

    if (pool == NULL || pool->fd < 0) {
        return 0;
    }

    /*
     * DMA_BUF_IOCTL_SYNC is the userspace-facing hook for CPU access to an
     * mmaped dma-buf. START says "the CPU is about to read/write this shared
     * buffer". For READ, the exporter may invalidate CPU cache or wait for a
     * producer fence. For WRITE, it may prepare ownership for CPU writes.
     *
     * On coherent x86 memory this can look like a no-op. On non-coherent SoCs
     * it is the sort of boundary where cache maintenance matters.
     */
    memset(&sync, 0, sizeof(sync));
    sync.flags = sync_flags(access, DMA_BUF_SYNC_START);
    if (ioctl(pool->fd, DMA_BUF_IOCTL_SYNC, &sync) != 0) {
        return -errno;
    }
    return 0;
}

int linky_pool_end_cpu_access(const linky_buffer_pool_t *pool, linky_cpu_access_t access)
{
    struct dma_buf_sync sync;

    if (pool == NULL || pool->fd < 0) {
        return 0;
    }

    /*
     * END says "the CPU is done". For WRITE, this is where dirty CPU cache
     * lines may need to become visible to the next device. For READ, it closes
     * the CPU access window.
     *
     * This lab has no real external importer, so the call mostly demonstrates
     * correct API shape. With a real V4L2/GPU/NPU importer, skipping this kind
     * of boundary can produce stale or partially visible data on platforms that
     * are not fully cache coherent.
     */
    memset(&sync, 0, sizeof(sync));
    sync.flags = sync_flags(access, DMA_BUF_SYNC_END);
    if (ioctl(pool->fd, DMA_BUF_IOCTL_SYNC, &sync) != 0) {
        return -errno;
    }
    return 0;
}

void linky_pool_destroy(linky_buffer_pool_t *pool)
{
    size_t total;

    if (pool == NULL) {
        return;
    }

    total = linky_pool_total_size(pool->count, pool->frame_size);
    if (pool->base != NULL) {
        if (pool->owns_mapping) {
            munmap(pool->base, total);
        } else {
            free(pool->base);
        }
    }
    if (pool->owns_fd && pool->fd >= 0) {
        close(pool->fd);
    }
    memset(pool, 0, sizeof(*pool));
    pool->fd = -1;
}

void *linky_pool_ptr(const linky_buffer_pool_t *pool, uint32_t index)
{
    if (pool == NULL || pool->base == NULL || index >= pool->count) {
        return NULL;
    }
    return (uint8_t *)pool->base + ((size_t)index * pool->frame_size);
}
