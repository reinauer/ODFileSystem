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
    int32_t  lru_prev;    /* previous entry in LRU list */
    int32_t  lru_next;    /* next entry in LRU/free list */
    int      valid;       /* is this entry populated? */
    uint8_t *data;        /* sector data */
} odfs_cache_entry_t;

/* block cache */
typedef struct odfs_cache {
    odfs_cache_entry_t *entries;
    int32_t            *buckets;     /* hash bucket -> entry index */
    int32_t            *next;        /* hash collision chain per entry */
    uint32_t             capacity;    /* number of entries */
    uint32_t             hash_size;
    uint32_t             valid_count;
    uint32_t             sector_size;
    uint32_t             sector_shift; /* log2(sector_size), 0 if not pow2 */
    uint32_t             clock;       /* LRU clock */
    int32_t              free_head;   /* first unused entry */
    int32_t              lru_head;    /* most recently used valid entry */
    int32_t              lru_tail;    /* least recently used valid entry */
    odfs_cache_stats_t  stats;
    odfs_media_t       *media;       /* underlying media for miss reads */
    uint8_t            *stream_buf;  /* staging for clustered miss reads;
                                        NULL disables read-ahead */
    uint32_t             last_miss_lba; /* detects sequential miss runs */
    uint32_t             generation;  /* bumped whenever a valid entry is
                                         replaced or dropped, so a caller
                                         holding a data pointer can tell
                                         it still holds the same sector */
} odfs_cache_t;

/* lifecycle */
odfs_err_t odfs_cache_init(odfs_cache_t *cache,
                             odfs_media_t *media,
                             uint32_t capacity);
void        odfs_cache_destroy(odfs_cache_t *cache);

/* is the sector cached? (does not count as a use or touch the LRU) */
int odfs_cache_contains(const odfs_cache_t *cache, uint32_t lba);

/*
 * Read an already cached sector without I/O. A hit counts as a read and
 * updates the LRU; a miss returns NULL without changing the cache or stats.
 * The returned data has the same lifetime as odfs_cache_read() data.
 */
const uint8_t *odfs_cache_read_cached(odfs_cache_t *cache, uint32_t lba);

#if ODFS_FEATURE_CACHE_RETAIN
/*
 * Retain a sector read by an external read-ahead request. Existing entries
 * are unchanged. This may evict the LRU entry but performs no media I/O and
 * does not count as a cache read. Data must point to one complete sector
 * outside the cache; callers must only supply successful, current-media
 * reads.
 */
odfs_err_t odfs_cache_retain(odfs_cache_t *cache, uint32_t lba,
                            const uint8_t *data);

#endif

/* read a single sector through the cache */
odfs_err_t odfs_cache_read(odfs_cache_t *cache,
                             uint32_t lba,
                             const uint8_t **out);

/*
 * Read bytes from a contiguous extent. Partial edge sectors and small reads
 * use the block cache; aligned runs of multiple full sectors bypass it so
 * streaming file reads can be coalesced by the media layer.
 */
odfs_err_t odfs_cache_read_bytes(odfs_cache_t *cache,
                                  uint32_t start_lba,
                                  uint64_t offset,
                                  void *buf,
                                  size_t *len);

#if !defined(AMIGA)
/* Host tools/tests only; Amiga mounts own the cache lifetime. */
/* invalidate all entries */
void odfs_cache_flush(odfs_cache_t *cache);

/* get current stats */
const odfs_cache_stats_t *odfs_cache_get_stats(const odfs_cache_t *cache);
#endif

#endif /* ODFS_CACHE_H */
