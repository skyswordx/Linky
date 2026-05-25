#define _GNU_SOURCE
#include "linky/linky.h"

#include "buffer_pool.h"
#include "ring.h"
#include "stats_private.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <unistd.h>

typedef struct linky_context {
    const linky_config_t *cfg;
    linky_buffer_pool_t pool;
    linky_ring_t ready;
    linky_ring_t freeq;
    int event_fd;
    _Atomic int producer_done;
    _Atomic uint64_t dropped;
    uint64_t *latencies;
    _Atomic uint64_t latency_count;
    uint64_t start_ns;
    uint64_t end_ns;
} linky_context_t;

static void apply_thread_policy(int cpu, int realtime_priority)
{
    if (cpu >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(cpu, &set);
        pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    }

    if (realtime_priority > 0) {
        struct sched_param param;
        memset(&param, 0, sizeof(param));
        param.sched_priority = realtime_priority;
        pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
    }
}

static void touch_frame(void *ptr, size_t size, uint32_t sequence)
{
    uint8_t *bytes = ptr;
    size_t step = 64;
    size_t i;

    for (i = 0; i < size; i += step) {
        bytes[i] = (uint8_t)(sequence + i);
    }
    if (size > 0) {
        bytes[size - 1] = (uint8_t)sequence;
    }
}

static uint8_t sample_frame(const void *ptr, size_t size)
{
    const uint8_t *bytes = ptr;

    if (size == 0) {
        return 0;
    }
    return (uint8_t)(bytes[0] ^ bytes[size - 1]);
}

static void wait_for_event(linky_context_t *ctx)
{
    uint64_t value;

    if (!ctx->cfg->use_eventfd || ctx->event_fd < 0) {
        sched_yield();
        return;
    }

    /*
     * eventfd is used here as a tiny fence-like teaching model:
     *
     * - producer writes the frame and descriptor;
     * - producer writes eventfd;
     * - consumer wakes and then pops the descriptor.
     *
     * A real dma-fence is a kernel object attached to GPU/V4L2/DRM work. It
     * represents completion of asynchronous hardware work. eventfd does not
     * carry cache semantics and does not prove device completion. It only lets
     * this lab demonstrate "do not consume before producer says ready".
     */
    if (read(ctx->event_fd, &value, sizeof(value)) < 0 && errno != EAGAIN) {
        sched_yield();
    }
}

static void notify_event(linky_context_t *ctx)
{
    uint64_t one = 1;

    if (!ctx->cfg->use_eventfd || ctx->event_fd < 0) {
        return;
    }

    if (write(ctx->event_fd, &one, sizeof(one)) < 0 && errno != EAGAIN) {
        return;
    }
}

static void *producer_main(void *arg)
{
    linky_context_t *ctx = arg;
    uint32_t sequence;
    linky_desc_t free_desc;

    apply_thread_policy(ctx->cfg->producer_cpu, ctx->cfg->realtime_priority);

    for (sequence = 0; sequence < ctx->cfg->iterations; ++sequence) {
        while (linky_ring_pop(&ctx->freeq, &free_desc) != 0) {
            sched_yield();
        }

        void *frame = linky_pool_ptr(&ctx->pool, free_desc.buffer_id);
        linky_desc_t ready = {
            .buffer_id = free_desc.buffer_id,
            .sequence = sequence,
            .size = (uint32_t)ctx->cfg->frame_size,
            .flags = 0,
            .timestamp_ns = linky_now_ns(),
            .offset = (uint64_t)free_desc.buffer_id * ctx->cfg->frame_size,
        };

        /*
         * If the backing store is a dma-buf and the CPU writes through mmap,
         * wrap the CPU access with DMA_BUF_IOCTL_SYNC START/END. The heap and
         * memfd experiments return immediately here because they are not
         * dma-buf fds.
         */
        linky_pool_begin_cpu_access(&ctx->pool, LINKY_CPU_ACCESS_WRITE);
        touch_frame(frame, ctx->cfg->frame_size, sequence);
        linky_pool_end_cpu_access(&ctx->pool, LINKY_CPU_ACCESS_WRITE);

        while (linky_ring_push(&ctx->ready, &ready) != 0) {
            atomic_fetch_add_explicit(&ctx->dropped, 1, memory_order_relaxed);
            sched_yield();
        }
        notify_event(ctx);
    }

    atomic_store_explicit(&ctx->producer_done, 1, memory_order_release);
    notify_event(ctx);
    return NULL;
}

static void *consumer_main(void *arg)
{
    linky_context_t *ctx = arg;
    uint64_t count = 0;

    apply_thread_policy(ctx->cfg->consumer_cpu, ctx->cfg->realtime_priority);

    while (1) {
        linky_desc_t item;
        int ret = linky_ring_pop(&ctx->ready, &item);

        if (ret != 0) {
            if (atomic_load_explicit(&ctx->producer_done, memory_order_acquire)) {
                if (linky_ring_pop(&ctx->ready, &item) != 0) {
                    break;
                }
            } else {
                wait_for_event(ctx);
                continue;
            }
        }

        if (count < LINKY_MAX_LATENCIES) {
            uint8_t checksum;
            void *frame = linky_pool_ptr(&ctx->pool, item.buffer_id);

            /*
             * The consumer samples the frame so the READ sync has a concrete
             * place in the code. In a real VPU/RGA/NPU chain, this stage might
             * be another device importer rather than the CPU.
             */
            linky_pool_begin_cpu_access(&ctx->pool, LINKY_CPU_ACCESS_READ);
            checksum = sample_frame(frame, ctx->cfg->frame_size);
            linky_pool_end_cpu_access(&ctx->pool, LINKY_CPU_ACCESS_READ);
            (void)checksum;

            ctx->latencies[count] = linky_now_ns() - item.timestamp_ns;
            count++;
        }

        while (linky_ring_push(&ctx->freeq, &item) != 0) {
            sched_yield();
        }
    }

    atomic_store_explicit(&ctx->latency_count, count, memory_order_release);
    return NULL;
}

static int allocate_pool_for_mode(linky_context_t *ctx)
{
    const linky_config_t *cfg = ctx->cfg;

    switch (cfg->mode) {
    case LINKY_MODE_POOL:
        return linky_pool_alloc_heap(&ctx->pool, cfg->buffer_count, cfg->frame_size);
    case LINKY_MODE_MEMFD:
        return linky_pool_alloc_memfd(&ctx->pool, cfg->buffer_count, cfg->frame_size);
    case LINKY_MODE_DMABUF:
        return linky_pool_alloc_dmabuf(&ctx->pool,
                                       cfg->buffer_count,
                                       cfg->frame_size,
                                       cfg->dma_heap_path);
    default:
        return -EINVAL;
    }
}

int linky_run(const linky_config_t *cfg, linky_stats_t *stats)
{
    linky_context_t ctx;
    pthread_t producer;
    pthread_t consumer;
    uint32_t i;
    int ret;

    if (cfg == NULL || stats == NULL || cfg->buffer_count < 2 ||
        cfg->ring_capacity < 2 || cfg->frame_size == 0 ||
        cfg->iterations == 0 || cfg->iterations > LINKY_MAX_LATENCIES) {
        return -EINVAL;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.cfg = cfg;
    ctx.event_fd = -1;

    if (cfg->use_mlockall) {
        mlockall(MCL_CURRENT | MCL_FUTURE);
    }

    ctx.latencies = calloc(cfg->iterations, sizeof(*ctx.latencies));
    if (ctx.latencies == NULL) {
        return -ENOMEM;
    }

    ret = allocate_pool_for_mode(&ctx);
    if (ret != 0) {
        free(ctx.latencies);
        return ret;
    }

    ret = linky_ring_init(&ctx.ready, cfg->ring_capacity);
    if (ret == 0) {
        ret = linky_ring_init(&ctx.freeq, cfg->buffer_count + 1u);
    }
    if (ret != 0) {
        linky_pool_destroy(&ctx.pool);
        free(ctx.latencies);
        return ret;
    }

    if (cfg->use_eventfd) {
        ctx.event_fd = eventfd(0, EFD_CLOEXEC);
        if (ctx.event_fd < 0) {
            ret = -errno;
            linky_ring_destroy(&ctx.ready);
            linky_ring_destroy(&ctx.freeq);
            linky_pool_destroy(&ctx.pool);
            free(ctx.latencies);
            return ret;
        }
    }

    for (i = 0; i < cfg->buffer_count; ++i) {
        linky_desc_t desc = {
            .buffer_id = i,
            .sequence = 0,
            .size = (uint32_t)cfg->frame_size,
            .flags = 0,
            .timestamp_ns = 0,
            .offset = (uint64_t)i * cfg->frame_size,
        };
        while (linky_ring_push(&ctx.freeq, &desc) != 0) {
            sched_yield();
        }
    }

    ctx.start_ns = linky_now_ns();
    ret = pthread_create(&consumer, NULL, consumer_main, &ctx);
    if (ret == 0) {
        ret = pthread_create(&producer, NULL, producer_main, &ctx);
    }
    if (ret != 0) {
        atomic_store_explicit(&ctx.producer_done, 1, memory_order_release);
        notify_event(&ctx);
        linky_ring_destroy(&ctx.ready);
        linky_ring_destroy(&ctx.freeq);
        linky_pool_destroy(&ctx.pool);
        free(ctx.latencies);
        if (ctx.event_fd >= 0) {
            close(ctx.event_fd);
        }
        return -ret;
    }

    pthread_join(producer, NULL);
    notify_event(&ctx);
    pthread_join(consumer, NULL);
    ctx.end_ns = linky_now_ns();

    linky_stats_from_samples(ctx.latencies,
                             atomic_load_explicit(&ctx.latency_count, memory_order_acquire),
                             atomic_load_explicit(&ctx.dropped, memory_order_acquire),
                             ctx.end_ns - ctx.start_ns,
                             stats);

    if (ctx.event_fd >= 0) {
        close(ctx.event_fd);
    }
    linky_ring_destroy(&ctx.ready);
    linky_ring_destroy(&ctx.freeq);
    linky_pool_destroy(&ctx.pool);
    free(ctx.latencies);
    return 0;
}
