/*
 * cache_block.c — LRU block cache
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "odfs/cache.h"
#include "odfs/alloc.h"
#include <string.h>

#define ODFS_CACHE_STREAM_MIN_SECTORS 2u

/*
 * Clustered miss reads: on a cache miss, read up to this many contiguous
 * sectors in one device request and install them all. Directory extents
 * and small sequential reads are contiguous, so this replaces N device
 * round trips with one. Only enabled for caches large enough that the
 * cluster cannot churn a meaningful share of the entries.
 */
#define ODFS_CACHE_READAHEAD 8u

static void cache_reset_indices(odfs_cache_t *cache)
{
    uint32_t i;

    if (!cache)
        return;

    if (cache->buckets) {
        for (i = 0; i < cache->hash_size; i++)
            cache->buckets[i] = -1;
    }
    if (cache->next) {
        for (i = 0; i < cache->capacity; i++)
            cache->next[i] = -1;
    }
    cache->free_head = cache->capacity ? 0 : -1;
    cache->lru_head = -1;
    cache->lru_tail = -1;
    if (cache->entries) {
        for (i = 0; i < cache->capacity; i++) {
            cache->entries[i].lru_prev = -1;
            cache->entries[i].lru_next =
                (i + 1u < cache->capacity) ? (int32_t)(i + 1u) : -1;
        }
    }
}

static uint32_t cache_hash_lba(const odfs_cache_t *cache, uint32_t lba)
{
    return cache->hash_size ? (lba % cache->hash_size) : 0;
}

static int32_t cache_find_index(const odfs_cache_t *cache, uint32_t lba)
{
    int32_t idx;

    if (!cache || !cache->buckets || !cache->next || cache->hash_size == 0)
        return -1;

    idx = cache->buckets[cache_hash_lba(cache, lba)];
    while (idx >= 0) {
        const odfs_cache_entry_t *entry = &cache->entries[idx];

        if (entry->valid && entry->lba == lba)
            return idx;
        idx = cache->next[idx];
    }
    return -1;
}

static void cache_insert_index(odfs_cache_t *cache, uint32_t idx)
{
    uint32_t bucket;

    if (!cache || !cache->buckets || !cache->next || idx >= cache->capacity)
        return;

    bucket = cache_hash_lba(cache, cache->entries[idx].lba);
    cache->next[idx] = cache->buckets[bucket];
    cache->buckets[bucket] = (int32_t)idx;
}

static void cache_remove_index(odfs_cache_t *cache, uint32_t idx)
{
    uint32_t bucket;
    int32_t cur;
    int32_t prev = -1;

    if (!cache || !cache->buckets || !cache->next || idx >= cache->capacity)
        return;

    bucket = cache_hash_lba(cache, cache->entries[idx].lba);
    cur = cache->buckets[bucket];
    while (cur >= 0) {
        if ((uint32_t)cur == idx) {
            if (prev >= 0)
                cache->next[prev] = cache->next[cur];
            else
                cache->buckets[bucket] = cache->next[cur];
            cache->next[cur] = -1;
            return;
        }
        prev = cur;
        cur = cache->next[cur];
    }
}

static void cache_lru_remove(odfs_cache_t *cache, uint32_t idx)
{
    odfs_cache_entry_t *entry;
    int32_t prev;
    int32_t next;

    if (!cache || !cache->entries || idx >= cache->capacity)
        return;

    entry = &cache->entries[idx];
    prev = entry->lru_prev;
    next = entry->lru_next;

    if (prev >= 0)
        cache->entries[prev].lru_next = next;
    else if (cache->lru_head == (int32_t)idx)
        cache->lru_head = next;

    if (next >= 0)
        cache->entries[next].lru_prev = prev;
    else if (cache->lru_tail == (int32_t)idx)
        cache->lru_tail = prev;

    entry->lru_prev = -1;
    entry->lru_next = -1;
}

static void cache_lru_insert_head(odfs_cache_t *cache, uint32_t idx)
{
    odfs_cache_entry_t *entry;

    if (!cache || !cache->entries || idx >= cache->capacity)
        return;

    entry = &cache->entries[idx];
    entry->lru_prev = -1;
    entry->lru_next = cache->lru_head;
    if (cache->lru_head >= 0)
        cache->entries[cache->lru_head].lru_prev = (int32_t)idx;
    else
        cache->lru_tail = (int32_t)idx;
    cache->lru_head = (int32_t)idx;
}

static void cache_lru_make_mru(odfs_cache_t *cache, uint32_t idx)
{
    if (!cache || cache->lru_head == (int32_t)idx)
        return;

    cache_lru_remove(cache, idx);
    cache_lru_insert_head(cache, idx);
}

static int32_t cache_pop_free(odfs_cache_t *cache)
{
    int32_t idx;

    if (!cache || !cache->entries)
        return -1;

    idx = cache->free_head;
    if (idx < 0)
        return -1;

    cache->free_head = cache->entries[idx].lru_next;
    cache->entries[idx].lru_next = -1;
    cache->entries[idx].lru_prev = -1;
    return idx;
}

odfs_err_t odfs_cache_init(odfs_cache_t *cache,
                             odfs_media_t *media,
                             uint32_t capacity)
{
    uint32_t sector_size;
    uint32_t i;
    size_t entries_bytes;
    size_t next_bytes;
    size_t bucket_bytes;

    if (!cache || !media || capacity == 0)
        return ODFS_ERR_INVAL;
    if (capacity > (UINT32_MAX - 1u) / 2u)
        return ODFS_ERR_RANGE;

    entries_bytes = (size_t)capacity * sizeof(odfs_cache_entry_t);
    next_bytes = (size_t)capacity * sizeof(cache->next[0]);
    if (entries_bytes / sizeof(odfs_cache_entry_t) != capacity ||
        next_bytes / sizeof(cache->next[0]) != capacity)
        return ODFS_ERR_OVERFLOW;

    memset(cache, 0, sizeof(*cache));
    sector_size = odfs_media_sector_size(media);
    if (sector_size == 0)
        return ODFS_ERR_INVAL;

    cache->entries = odfs_calloc(capacity, sizeof(odfs_cache_entry_t));
    if (!cache->entries)
        return ODFS_ERR_NOMEM;

    cache->hash_size = capacity * 2u + 1u;
    bucket_bytes = (size_t)cache->hash_size * sizeof(cache->buckets[0]);
    if (bucket_bytes / sizeof(cache->buckets[0]) != cache->hash_size) {
        odfs_free(cache->entries);
        memset(cache, 0, sizeof(*cache));
        return ODFS_ERR_OVERFLOW;
    }
    cache->buckets = odfs_malloc(bucket_bytes);
    cache->next = odfs_malloc(next_bytes);
    if (!cache->buckets || !cache->next) {
        odfs_free(cache->buckets);
        odfs_free(cache->next);
        odfs_free(cache->entries);
        memset(cache, 0, sizeof(*cache));
        return ODFS_ERR_NOMEM;
    }

    /* allocate data buffers for each entry */
    for (i = 0; i < capacity; i++) {
        cache->entries[i].data = odfs_malloc(sector_size);
        if (!cache->entries[i].data) {
            /* roll back */
            for (uint32_t j = 0; j < i; j++)
                odfs_free(cache->entries[j].data);
            odfs_free(cache->buckets);
            odfs_free(cache->next);
            odfs_free(cache->entries);
            memset(cache, 0, sizeof(*cache));
            return ODFS_ERR_NOMEM;
        }
        cache->entries[i].valid = 0;
    }

    cache->capacity = capacity;
    cache->sector_size = sector_size;
    cache->clock = 0;
    cache->media = media;
    cache->stream_buf = NULL;
    if (capacity >= 4u * ODFS_CACHE_READAHEAD) {
        /* optional: read-ahead is skipped when this allocation fails */
        cache->stream_buf = odfs_malloc((size_t)ODFS_CACHE_READAHEAD *
                                        sector_size);
    }
    cache_reset_indices(cache);

    return ODFS_OK;
}

void odfs_cache_destroy(odfs_cache_t *cache)
{
    if (!cache || !cache->entries)
        return;

    for (uint32_t i = 0; i < cache->capacity; i++)
        odfs_free(cache->entries[i].data);
    odfs_free(cache->stream_buf);
    odfs_free(cache->buckets);
    odfs_free(cache->next);
    odfs_free(cache->entries);

    memset(cache, 0, sizeof(*cache));
}

void odfs_cache_flush(odfs_cache_t *cache)
{
    if (!cache || !cache->entries)
        return;

    for (uint32_t i = 0; i < cache->capacity; i++)
        cache->entries[i].valid = 0;
    cache->valid_count = 0;
    cache_reset_indices(cache);
}

/* place one sector's data into a victim entry and index it */
static int32_t cache_install(odfs_cache_t *cache, uint32_t lba,
                             const uint8_t *src)
{
    uint32_t victim;
    int victim_valid;

    if (cache->valid_count < cache->capacity) {
        if (cache->free_head < 0)
            return -1;
        victim = (uint32_t)cache->free_head;
    } else {
        if (cache->lru_tail < 0)
            return -1;
        victim = (uint32_t)cache->lru_tail;
    }

    victim_valid = cache->entries[victim].valid;
    memcpy(cache->entries[victim].data, src, cache->sector_size);

    if (victim_valid) {
        cache_remove_index(cache, victim);
        cache_lru_remove(cache, victim);
        cache->stats.evictions++;
    } else {
        int32_t free_idx = cache_pop_free(cache);

        if (free_idx != (int32_t)victim)
            return -1;
        cache->valid_count++;
    }

    cache->entries[victim].lba = lba;
    cache->entries[victim].age = cache->clock;
    cache->entries[victim].valid = 1;
    cache_insert_index(cache, victim);
    cache_lru_insert_head(cache, victim);

    if (cache->valid_count > cache->stats.max_used)
        cache->stats.max_used = cache->valid_count;

    return (int32_t)victim;
}

odfs_err_t odfs_cache_read(odfs_cache_t *cache,
                             uint32_t lba,
                             const uint8_t **out)
{
    uint32_t victim = 0;
    int victim_valid;
    int32_t hit;
    odfs_err_t err;

    if (!cache || !cache->entries || !out)
        return ODFS_ERR_INVAL;

    cache->stats.reads++;
    cache->clock++;

    hit = cache_find_index(cache, lba);
    if (hit >= 0) {
        cache->entries[hit].age = cache->clock;
        cache_lru_make_mru(cache, (uint32_t)hit);
        cache->stats.hits++;
        *out = cache->entries[hit].data;
        return ODFS_OK;
    }

    /* miss — find victim (LRU or first invalid) */
    cache->stats.misses++;

    /* Cluster only sequential miss patterns (directory scans, small
     * sequential reads); random misses would fetch data that is
     * evicted unused while paying for the larger transfer. */
    if (cache->stream_buf && lba == cache->last_miss_lba + 1u) {
        uint32_t run = 1;

        while (run < ODFS_CACHE_READAHEAD &&
               lba + run > lba &&
               cache_find_index(cache, lba + run) < 0)
            run++;

        if (run > 1 &&
            odfs_media_read(cache->media, lba, run,
                            cache->stream_buf) == ODFS_OK) {
            uint32_t i;
            int32_t idx = -1;

            /* install the requested sector last so the later installs
             * cannot evict it before it is returned */
            for (i = run; i-- > 0; ) {
                idx = cache_install(cache, lba + i,
                                    cache->stream_buf +
                                    (size_t)i * cache->sector_size);
                if (idx < 0)
                    break;
            }
            if (idx >= 0) {
                cache->last_miss_lba = lba + run - 1u;
                *out = cache->entries[idx].data;
                return ODFS_OK;
            }
        }
        /* short run, media error (e.g. the run crosses the end of the
         * disc), or install failure: fall back to a single sector */
    }

    cache->last_miss_lba = lba;

    if (cache->valid_count < cache->capacity) {
        if (cache->free_head < 0)
            return ODFS_ERR_CORRUPT;
        victim = (uint32_t)cache->free_head;
    } else {
        if (cache->lru_tail < 0)
            return ODFS_ERR_CORRUPT;
        victim = (uint32_t)cache->lru_tail;
    }
    victim_valid = cache->entries[victim].valid;
    err = odfs_media_read(cache->media, lba, 1, cache->entries[victim].data);
    if (err != ODFS_OK)
        return err;

    if (victim_valid) {
        cache_remove_index(cache, victim);
        cache_lru_remove(cache, victim);
        cache->stats.evictions++;
    } else {
        int32_t free_idx = cache_pop_free(cache);
        if (free_idx != (int32_t)victim)
            return ODFS_ERR_CORRUPT;
        cache->valid_count++;
    }

    cache->entries[victim].lba = lba;
    cache->entries[victim].age = cache->clock;
    cache->entries[victim].valid = 1;
    cache_insert_index(cache, victim);
    cache_lru_insert_head(cache, victim);

    if (cache->valid_count > cache->stats.max_used)
        cache->stats.max_used = cache->valid_count;

    *out = cache->entries[victim].data;
    return ODFS_OK;
}

static odfs_err_t cache_copy_sector(odfs_cache_t *cache,
                                    uint32_t lba,
                                    uint32_t offset,
                                    uint8_t *out,
                                    size_t len)
{
    const uint8_t *sector;
    odfs_err_t err;

    err = odfs_cache_read(cache, lba, &sector);
    if (err != ODFS_OK)
        return err;

    memcpy(out, sector + offset, len);
    return ODFS_OK;
}

odfs_err_t odfs_cache_read_bytes(odfs_cache_t *cache,
                                  uint32_t start_lba,
                                  uint64_t offset,
                                  void *buf,
                                  size_t *len)
{
    uint8_t *out = buf;
    uint32_t sector_size;
    uint64_t lba64;
    uint32_t lba;
    uint32_t sector_off;
    size_t want;
    size_t done = 0;
    odfs_err_t err;

    if (!cache || !cache->entries || !cache->media || !buf || !len)
        return ODFS_ERR_INVAL;

    sector_size = cache->sector_size;
    if (sector_size == 0)
        return ODFS_ERR_INVAL;

    want = *len;
    if (want == 0)
        return ODFS_OK;

    lba64 = (uint64_t)start_lba + offset / sector_size;
    if (lba64 > UINT32_MAX)
        return ODFS_ERR_RANGE;
    lba = (uint32_t)lba64;
    sector_off = (uint32_t)(offset % sector_size);

    if (sector_off != 0) {
        size_t chunk = sector_size - sector_off;

        if (chunk > want)
            chunk = want;

        err = cache_copy_sector(cache, lba, sector_off, out, chunk);
        if (err != ODFS_OK) {
            *len = done;
            return err;
        }

        done += chunk;
        out += chunk;
        if (done < want && lba == UINT32_MAX) {
            *len = done;
            return ODFS_ERR_RANGE;
        }
        lba++;
    }

    while (want - done >= sector_size) {
        size_t full_sectors = (want - done) / sector_size;
        uint64_t next_lba;
        uint32_t count;
        size_t bytes;

        if (full_sectors < ODFS_CACHE_STREAM_MIN_SECTORS)
            break;

        count = UINT32_MAX;
        if ((uint64_t)full_sectors < (uint64_t)count)
            count = (uint32_t)full_sectors;

        if ((size_t)count > ((size_t)-1) / sector_size)
            count = (uint32_t)(((size_t)-1) / sector_size);
        if (count == 0)
            return ODFS_ERR_OVERFLOW;
        if ((uint64_t)count > (uint64_t)UINT32_MAX - lba + 1u)
            count = (uint32_t)((uint64_t)UINT32_MAX - lba + 1u);

        err = odfs_media_read(cache->media, lba, count, out);
        if (err != ODFS_OK) {
            *len = done;
            return err;
        }

        bytes = (size_t)count * sector_size;
        done += bytes;
        out += bytes;
        next_lba = (uint64_t)lba + count;
        if (done < want && next_lba > UINT32_MAX) {
            *len = done;
            return ODFS_ERR_RANGE;
        }
        lba = (uint32_t)next_lba;
    }

    while (want - done >= sector_size) {
        err = cache_copy_sector(cache, lba, 0, out, sector_size);
        if (err != ODFS_OK) {
            *len = done;
            return err;
        }
        done += sector_size;
        out += sector_size;
        if (done < want && lba == UINT32_MAX) {
            *len = done;
            return ODFS_ERR_RANGE;
        }
        lba++;
    }

    if (done < want) {
        size_t tail = want - done;

        err = cache_copy_sector(cache, lba, 0, out, tail);
        if (err != ODFS_OK) {
            *len = done;
            return err;
        }
        done += tail;
    }

    *len = done;
    return ODFS_OK;
}

const odfs_cache_stats_t *odfs_cache_get_stats(const odfs_cache_t *cache)
{
    return &cache->stats;
}
