#include "ring.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

uint32_t linky_next_power_of_two(uint32_t value)
{
    uint32_t power = 1;

    if (value == 0) {
        return 1;
    }

    while (power < value && power <= (UINT32_MAX / 2u)) {
        power <<= 1u;
    }
    return power;
}

int linky_ring_init(linky_ring_t *ring, uint32_t capacity)
{
    uint32_t actual;

    if (ring == NULL || capacity < 2) {
        return -EINVAL;
    }

    memset(ring, 0, sizeof(*ring));
    actual = linky_next_power_of_two(capacity);
    ring->items = calloc(actual, sizeof(*ring->items));
    if (ring->items == NULL) {
        return -ENOMEM;
    }

    ring->capacity = actual;
    ring->mask = actual - 1u;
    atomic_store_explicit(&ring->head, 0, memory_order_relaxed);
    atomic_store_explicit(&ring->tail, 0, memory_order_relaxed);
    return 0;
}

void linky_ring_destroy(linky_ring_t *ring)
{
    if (ring == NULL) {
        return;
    }
    free(ring->items);
    memset(ring, 0, sizeof(*ring));
}

int linky_ring_push(linky_ring_t *ring, const linky_desc_t *item)
{
    uint32_t head = atomic_load_explicit(&ring->head, memory_order_relaxed);
    uint32_t next = (head + 1u) & ring->mask;
    uint32_t tail = atomic_load_explicit(&ring->tail, memory_order_acquire);

    if (next == tail) {
        return -EAGAIN;
    }

    /*
     * Store descriptor first, then publish the new head with release ordering.
     * The consumer uses acquire ordering when reading head, so it will not see
     * the descriptor as ready before the descriptor fields are visible.
     *
     * This is CPU-to-CPU memory ordering. It is not DMA cache synchronization.
     * Device visibility still needs DMA API / DMA-BUF sync at ownership
     * boundaries.
     */
    ring->items[head] = *item;
    atomic_store_explicit(&ring->head, next, memory_order_release);
    return 0;
}

int linky_ring_pop(linky_ring_t *ring, linky_desc_t *item)
{
    uint32_t tail = atomic_load_explicit(&ring->tail, memory_order_relaxed);
    uint32_t head = atomic_load_explicit(&ring->head, memory_order_acquire);

    if (tail == head) {
        return -EAGAIN;
    }

    /*
     * acquire load of head above pairs with producer's release store. After
     * that, reading the descriptor is safe from a CPU memory-ordering point of
     * view.
     */
    *item = ring->items[tail];
    atomic_store_explicit(&ring->tail, (tail + 1u) & ring->mask, memory_order_release);
    return 0;
}
