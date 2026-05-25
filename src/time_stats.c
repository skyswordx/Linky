#define _GNU_SOURCE
#include "linky/linky.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint64_t linky_now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0) {
        clock_gettime(CLOCK_MONOTONIC, &ts);
    }
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int linky_parse_u32(const char *text, uint32_t *out)
{
    char *end = NULL;
    unsigned long value;

    if (text == NULL || *text == '\0' || out == NULL) {
        return -EINVAL;
    }

    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value > UINT32_MAX) {
        return -EINVAL;
    }

    *out = (uint32_t)value;
    return 0;
}

int linky_parse_size(const char *text, size_t *out)
{
    char *end = NULL;
    unsigned long long value;
    unsigned long long multiplier = 1;

    if (text == NULL || *text == '\0' || out == NULL) {
        return -EINVAL;
    }

    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno != 0 || end == text) {
        return -EINVAL;
    }

    if (*end != '\0') {
        if (end[1] != '\0') {
            return -EINVAL;
        }
        switch (*end) {
        case 'k':
        case 'K':
            multiplier = 1024ull;
            break;
        case 'm':
        case 'M':
            multiplier = 1024ull * 1024ull;
            break;
        case 'g':
        case 'G':
            multiplier = 1024ull * 1024ull * 1024ull;
            break;
        default:
            return -EINVAL;
        }
    }

    if (value > SIZE_MAX / multiplier) {
        return -EINVAL;
    }

    *out = (size_t)(value * multiplier);
    return 0;
}

static int compare_u64(const void *a, const void *b)
{
    uint64_t va = *(const uint64_t *)a;
    uint64_t vb = *(const uint64_t *)b;
    return (va > vb) - (va < vb);
}

static double percentile_us(const uint64_t *sorted, uint64_t count, double pct)
{
    uint64_t index;

    if (count == 0) {
        return 0.0;
    }

    index = (uint64_t)((pct / 100.0) * (double)(count - 1));
    if (index >= count) {
        index = count - 1;
    }
    return (double)sorted[index] / 1000.0;
}

void linky_print_stats(const linky_config_t *cfg, const linky_stats_t *stats)
{
    printf("mode=%s frame_size=%zu buffers=%u iterations=%u eventfd=%s\n",
           linky_mode_name(cfg->mode),
           cfg->frame_size,
           cfg->buffer_count,
           cfg->iterations,
           cfg->use_eventfd ? "on" : "off");
    printf("samples=%" PRIu64 " dropped=%" PRIu64 " elapsed_ms=%.3f\n",
           stats->samples,
           stats->dropped,
           (double)stats->elapsed_ns / 1000000.0);
    printf("latency_us avg=%.3f p50=%.3f p95=%.3f p99=%.3f max=%.3f\n",
           stats->avg_us,
           stats->p50_us,
           stats->p95_us,
           stats->p99_us,
           stats->max_us);
}

const char *linky_mode_name(linky_mode_t mode)
{
    switch (mode) {
    case LINKY_MODE_POOL:
        return "pool";
    case LINKY_MODE_MEMFD:
        return "memfd";
    case LINKY_MODE_DMABUF:
        return "dmabuf";
    default:
        return "unknown";
    }
}

void linky_stats_from_samples(const uint64_t *samples,
                              uint64_t count,
                              uint64_t dropped,
                              uint64_t elapsed_ns,
                              linky_stats_t *stats)
{
    uint64_t *copy;
    uint64_t sum = 0;
    uint64_t max = 0;
    uint64_t i;

    memset(stats, 0, sizeof(*stats));
    stats->samples = count;
    stats->dropped = dropped;
    stats->elapsed_ns = elapsed_ns;

    if (count == 0) {
        return;
    }

    copy = malloc((size_t)count * sizeof(*copy));
    if (copy == NULL) {
        return;
    }
    memcpy(copy, samples, (size_t)count * sizeof(*copy));

    for (i = 0; i < count; ++i) {
        sum += samples[i];
        if (samples[i] > max) {
            max = samples[i];
        }
    }

    qsort(copy, (size_t)count, sizeof(*copy), compare_u64);
    stats->avg_us = (double)sum / (double)count / 1000.0;
    stats->p50_us = percentile_us(copy, count, 50.0);
    stats->p95_us = percentile_us(copy, count, 95.0);
    stats->p99_us = percentile_us(copy, count, 99.0);
    stats->max_us = (double)max / 1000.0;

    free(copy);
}
