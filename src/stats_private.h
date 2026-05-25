#ifndef LINKY_STATS_PRIVATE_H
#define LINKY_STATS_PRIVATE_H

#include "linky/linky.h"

void linky_stats_from_samples(const uint64_t *samples,
                              uint64_t count,
                              uint64_t dropped,
                              uint64_t elapsed_ns,
                              linky_stats_t *stats);

#endif
