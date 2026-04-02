/*
 * odfs/cache.h — block cache
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef ODFS_CACHE_H
#define ODFS_CACHE_H

#include "odfs/config.h"
#include "odfs/error.h"
#include "odfs/media.h"
#include <stdint.h>
#include <stddef.h>

/* cache telemetry counters */
typedef struct odfs_cache_stats {
    uint32_t reads;       /* total read requests */
    uint32_t hits;        /* served from cache */
    uint32_t misses;      /* required I/O */
    uint32_t evictions;   /* entries evicted */
    uint32_t max_used;    /* high-water mark of entries used */
} odfs_cache_stats_t;

/* single cache entry */
typedef struct odfs_cache_entry {
    uint32_t lba;         /* sector this entry holds */
    uint32_t age;         /* for LRU: incremented each access cycle */
    int      valid;       /* is this entry populated? */
    uint8_t *data;        /* sector data */
} odfs_cache_entry_t;

/* block cache */
typedef struct odfs_cache {
    odfs_cache_entry_t *entries;
    uint32_t             capacity;    /* number of entries */
    uint32_t             sector_size;
    uint32_t             clock;       /* LRU clock */
    odfs_cache_stats_t  stats;
    odfs_media_t       *media;       /* underlying media for miss reads */
} odfs_cache_t;

/* lifecycle */
odfs_err_t odfs_cache_init(odfs_cache_t *cache,
                             odfs_media_t *media,
                             uint32_t capacity);
void        odfs_cache_destroy(odfs_cache_t *cache);

/* read a single sector through the cache */
odfs_err_t odfs_cache_read(odfs_cache_t *cache,
                             uint32_t lba,
                             const uint8_t **out);

/* invalidate all entries */
void odfs_cache_flush(odfs_cache_t *cache);

/* get current stats */
const odfs_cache_stats_t *odfs_cache_get_stats(const odfs_cache_t *cache);

#endif /* ODFS_CACHE_H */
