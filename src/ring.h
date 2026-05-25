#ifndef LINKY_RING_H
#define LINKY_RING_H

#include "linky/linky.h"

#include <stdatomic.h>
#include <stdint.h>

typedef struct linky_ring {
    /*
     * Single-producer single-consumer ring.
     *
     * In a low-latency video path, queues should normally carry descriptors,
     * not frame bytes. The descriptor says "buffer N is ready, here is the
     * timestamp and metadata". The large frame stays in the preallocated pool.
     *
     * head and tail live on separate cache lines to reduce false sharing
     * between producer and consumer cores.
     */
    uint32_t capacity;
    uint32_t mask;
    _Atomic uint32_t head;
    char pad0[LINKY_CACHELINE - sizeof(_Atomic uint32_t)];
    _Atomic uint32_t tail;
    char pad1[LINKY_CACHELINE - sizeof(_Atomic uint32_t)];
    linky_desc_t *items;
} linky_ring_t;

int linky_ring_init(linky_ring_t *ring, uint32_t capacity);
void linky_ring_destroy(linky_ring_t *ring);
int linky_ring_push(linky_ring_t *ring, const linky_desc_t *item);
int linky_ring_pop(linky_ring_t *ring, linky_desc_t *item);
uint32_t linky_next_power_of_two(uint32_t value);

#endif
