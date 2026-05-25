#ifndef LINKY_H
#define LINKY_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LINKY_CACHELINE 64u
#define LINKY_MAX_LATENCIES 1000000u

typedef enum linky_mode {
    LINKY_MODE_POOL = 0,
    LINKY_MODE_MEMFD = 1,
    LINKY_MODE_DMABUF = 2
} linky_mode_t;

typedef struct linky_config {
    linky_mode_t mode;
    /* Size of one fake video frame. Real systems would also carry width,
     * height, format, plane stride and plane offsets. */
    size_t frame_size;
    /* Number of buffers preallocated before the hot path starts. */
    uint32_t buffer_count;
    /* Number of descriptors the producer will publish. */
    uint32_t iterations;
    /* Descriptor queue capacity. The ring carries metadata, not frame bytes. */
    uint32_t ring_capacity;
    /* eventfd models a CPU-side ready notification. It is not a dma-fence. */
    int use_eventfd;
    /* Lock current/future mappings to reduce page-fault jitter where allowed. */
    int use_mlockall;
    int producer_cpu;
    int consumer_cpu;
    int realtime_priority;
    const char *dma_heap_path;
} linky_config_t;

typedef struct linky_stats {
    uint64_t samples;
    uint64_t dropped;
    uint64_t elapsed_ns;
    double avg_us;
    double p50_us;
    double p95_us;
    double p99_us;
    double max_us;
} linky_stats_t;

typedef struct linky_desc {
    /* Index into the preallocated buffer pool. */
    uint32_t buffer_id;
    /* Monotonic frame number, useful for dropped-frame detection. */
    uint32_t sequence;
    /* Valid payload bytes in the buffer. */
    uint32_t size;
    uint32_t flags;
    /* Producer timestamp; consumer subtracts this to build latency samples. */
    uint64_t timestamp_ns;
    /* Offset inside fd-backed backing store. This mirrors real descriptor APIs. */
    uint64_t offset;
} linky_desc_t;

uint64_t linky_now_ns(void);
int linky_parse_u32(const char *text, uint32_t *out);
int linky_parse_size(const char *text, size_t *out);

int linky_run(const linky_config_t *cfg, linky_stats_t *stats);
void linky_print_stats(const linky_config_t *cfg, const linky_stats_t *stats);
const char *linky_mode_name(linky_mode_t mode);

int linky_selftest(void);

#ifdef __cplusplus
}
#endif

#endif
