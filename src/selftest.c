#include "linky/linky.h"

#include "buffer_pool.h"
#include "ring.h"
#include "stats_private.h"

#include <stdio.h>
#include <string.h>

static int expect_int(const char *name, int actual, int expected)
{
    if (actual != expected) {
        fprintf(stderr, "FAIL %s: got %d expected %d\n", name, actual, expected);
        return 1;
    }
    return 0;
}

static int test_parse_size(void)
{
    size_t value = 0;
    int fails = 0;

    fails += expect_int("parse 64", linky_parse_size("64", &value), 0);
    fails += value == 64 ? 0 : 1;
    fails += expect_int("parse 2K", linky_parse_size("2K", &value), 0);
    fails += value == 2048 ? 0 : 1;
    fails += expect_int("parse bad", linky_parse_size("12XB", &value), -22);
    return fails;
}

static int test_ring_order(void)
{
    linky_ring_t ring;
    linky_desc_t in = {0};
    linky_desc_t out = {0};
    int fails = 0;

    fails += expect_int("ring init", linky_ring_init(&ring, 4), 0);
    in.buffer_id = 7;
    in.sequence = 42;
    fails += expect_int("ring push", linky_ring_push(&ring, &in), 0);
    fails += expect_int("ring pop", linky_ring_pop(&ring, &out), 0);
    fails += out.buffer_id == 7 && out.sequence == 42 ? 0 : 1;
    fails += expect_int("ring empty", linky_ring_pop(&ring, &out), -11);
    linky_ring_destroy(&ring);
    return fails;
}

static int test_pool_offsets(void)
{
    linky_buffer_pool_t pool;
    int fails = 0;

    fails += expect_int("pool alloc", linky_pool_alloc_heap(&pool, 4, 128), 0);
    fails += ((char *)linky_pool_ptr(&pool, 2) - (char *)linky_pool_ptr(&pool, 0)) == 256 ? 0 : 1;
    memset(linky_pool_ptr(&pool, 1), 0x5a, 128);
    linky_pool_destroy(&pool);
    return fails;
}

static int test_stats(void)
{
    uint64_t samples[] = {1000, 2000, 3000, 4000, 5000};
    linky_stats_t stats;

    linky_stats_from_samples(samples, 5, 1, 1000000, &stats);
    if (stats.samples != 5 || stats.dropped != 1 || stats.p50_us != 3.0 || stats.max_us != 5.0) {
        fprintf(stderr, "FAIL stats\n");
        return 1;
    }
    return 0;
}

int linky_selftest(void)
{
    int fails = 0;

    fails += test_parse_size();
    fails += test_ring_order();
    fails += test_pool_offsets();
    fails += test_stats();

    if (fails != 0) {
        fprintf(stderr, "selftest failed: %d\n", fails);
        return 1;
    }

    puts("selftest passed");
    return 0;
}
