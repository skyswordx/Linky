#ifndef LINKY_BUFFER_POOL_H
#define LINKY_BUFFER_POOL_H

#include "linky/linky.h"

#include <stddef.h>

typedef struct linky_buffer_pool {
    void *base;
    size_t frame_size;
    uint32_t count;
    int fd;
    int owns_mapping;
    int owns_fd;
} linky_buffer_pool_t;

typedef enum linky_cpu_access {
    LINKY_CPU_ACCESS_READ = 1,
    LINKY_CPU_ACCESS_WRITE = 2
} linky_cpu_access_t;

int linky_pool_alloc_heap(linky_buffer_pool_t *pool, uint32_t count, size_t frame_size);
int linky_pool_alloc_memfd(linky_buffer_pool_t *pool, uint32_t count, size_t frame_size);
int linky_pool_alloc_dmabuf(linky_buffer_pool_t *pool,
                            uint32_t count,
                            size_t frame_size,
                            const char *heap_path);
int linky_pool_begin_cpu_access(const linky_buffer_pool_t *pool, linky_cpu_access_t access);
int linky_pool_end_cpu_access(const linky_buffer_pool_t *pool, linky_cpu_access_t access);
void linky_pool_destroy(linky_buffer_pool_t *pool);
void *linky_pool_ptr(const linky_buffer_pool_t *pool, uint32_t index);
size_t linky_pool_total_size(uint32_t count, size_t frame_size);

#endif
