#include "linky/linky.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static void print_usage(const char *argv0)
{
    printf("Usage: %s <pool|memfd|dmabuf|selftest> [options]\n", argv0);
    puts("");
    puts("Options:");
    puts("  --frames N          iterations, default 10000");
    puts("  --frame-size SIZE   bytes per frame, supports K/M/G, default 1M");
    puts("  --buffers N         buffer pool size, default 8");
    puts("  --ring N            descriptor ring capacity, default 64");
    puts("  --no-eventfd        busy-yield instead of eventfd notification");
    puts("  --mlockall          call mlockall(MCL_CURRENT | MCL_FUTURE)");
    puts("  --producer-cpu N    bind producer thread to CPU N");
    puts("  --consumer-cpu N    bind consumer thread to CPU N");
    puts("  --rt-prio N         request SCHED_FIFO priority N");
    puts("  --heap PATH         dma-buf heap path, default /dev/dma_heap/system");
    puts("  --csv              print one CSV row instead of human text");
    puts("");
    puts("Examples:");
    puts("  linky pool --frames 20000 --frame-size 1M --buffers 8");
    puts("  linky memfd --frames 20000 --mlockall --producer-cpu 2 --consumer-cpu 3");
    puts("  sudo linky dmabuf --heap /dev/dma_heap/system");
}

static int parse_mode(const char *text, linky_mode_t *mode)
{
    if (strcmp(text, "pool") == 0) {
        *mode = LINKY_MODE_POOL;
        return 0;
    }
    if (strcmp(text, "memfd") == 0) {
        *mode = LINKY_MODE_MEMFD;
        return 0;
    }
    if (strcmp(text, "dmabuf") == 0) {
        *mode = LINKY_MODE_DMABUF;
        return 0;
    }
    return -EINVAL;
}

static int parse_args(int argc, char **argv, linky_config_t *cfg)
{
    int i;

    if (argc < 2) {
        return -EINVAL;
    }
    if (parse_mode(argv[1], &cfg->mode) != 0) {
        return -EINVAL;
    }

    for (i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            if (linky_parse_u32(argv[++i], &cfg->iterations) != 0) {
                return -EINVAL;
            }
        } else if (strcmp(argv[i], "--frame-size") == 0 && i + 1 < argc) {
            if (linky_parse_size(argv[++i], &cfg->frame_size) != 0) {
                return -EINVAL;
            }
        } else if (strcmp(argv[i], "--buffers") == 0 && i + 1 < argc) {
            if (linky_parse_u32(argv[++i], &cfg->buffer_count) != 0) {
                return -EINVAL;
            }
        } else if (strcmp(argv[i], "--ring") == 0 && i + 1 < argc) {
            if (linky_parse_u32(argv[++i], &cfg->ring_capacity) != 0) {
                return -EINVAL;
            }
        } else if (strcmp(argv[i], "--no-eventfd") == 0) {
            cfg->use_eventfd = 0;
        } else if (strcmp(argv[i], "--mlockall") == 0) {
            cfg->use_mlockall = 1;
        } else if (strcmp(argv[i], "--producer-cpu") == 0 && i + 1 < argc) {
            uint32_t cpu;
            if (linky_parse_u32(argv[++i], &cpu) != 0) {
                return -EINVAL;
            }
            cfg->producer_cpu = (int)cpu;
        } else if (strcmp(argv[i], "--consumer-cpu") == 0 && i + 1 < argc) {
            uint32_t cpu;
            if (linky_parse_u32(argv[++i], &cpu) != 0) {
                return -EINVAL;
            }
            cfg->consumer_cpu = (int)cpu;
        } else if (strcmp(argv[i], "--rt-prio") == 0 && i + 1 < argc) {
            uint32_t prio;
            if (linky_parse_u32(argv[++i], &prio) != 0) {
                return -EINVAL;
            }
            cfg->realtime_priority = (int)prio;
        } else if (strcmp(argv[i], "--heap") == 0 && i + 1 < argc) {
            cfg->dma_heap_path = argv[++i];
        } else if (strcmp(argv[i], "--csv") == 0) {
            cfg->output_csv = 1;
        } else {
            return -EINVAL;
        }
    }

    return 0;
}

int main(int argc, char **argv)
{
    linky_config_t cfg = {
        .mode = LINKY_MODE_POOL,
        .frame_size = 1024u * 1024u,
        .buffer_count = 8,
        .iterations = 10000,
        .ring_capacity = 64,
        .use_eventfd = 1,
        .use_mlockall = 0,
        .producer_cpu = -1,
        .consumer_cpu = -1,
        .realtime_priority = 0,
        .output_csv = 0,
        .dma_heap_path = "/dev/dma_heap/system",
    };
    linky_stats_t stats;
    int ret;

    if (argc >= 2 && strcmp(argv[1], "selftest") == 0) {
        return linky_selftest();
    }

    ret = parse_args(argc, argv, &cfg);
    if (ret != 0) {
        print_usage(argv[0]);
        return 2;
    }

    /*
     * This executable is intentionally one binary with several modes. That
     * makes it easier to run the same workload across heap, memfd and dma-buf
     * backends and compare p95/p99 instead of reading separate demos.
     */
    ret = linky_run(&cfg, &stats);
    if (ret != 0) {
        fprintf(stderr, "linky_run failed: %s (%d)\n", strerror(-ret), ret);
        if (cfg.mode == LINKY_MODE_DMABUF) {
            fprintf(stderr, "hint: check whether %s exists and is readable\n", cfg.dma_heap_path);
        }
        return 1;
    }

    if (cfg.output_csv) {
        linky_print_csv_row(&cfg, &stats);
    } else {
        linky_print_stats(&cfg, &stats);
    }
    return 0;
}
