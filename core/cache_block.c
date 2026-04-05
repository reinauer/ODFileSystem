/*
 * cache_block.c — LRU block cache
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "odfs/cache.h"
#include "odfs/alloc.h"
#include <string.h>

odfs_err_t odfs_cache_init(odfs_cache_t *cache,
                             odfs_media_t *media,
                             uint32_t capacity)
{
    uint32_t sector_size;

    if (!cache || !media || capacity == 0)
        return ODFS_ERR_INVAL;

    memset(cache, 0, sizeof(*cache));
    sector_size = odfs_media_sector_size(media);
    if (sector_size == 0)
        return ODFS_ERR_INVAL;

    cache->entries = odfs_calloc(capacity, sizeof(odfs_cache_entry_t));
    if (!cache->entries)
        return ODFS_ERR_NOMEM;

    /* allocate data buffers for each entry */
    for (uint32_t i = 0; i < capacity; i++) {
        cache->entries[i].data = odfs_malloc(sector_size);
        if (!cache->entries[i].data) {
            /* roll back */
            for (uint32_t j = 0; j < i; j++)
                odfs_free(cache->entries[j].data);
            odfs_free(cache->entries);
            cache->entries = NULL;
            return ODFS_ERR_NOMEM;
        }
        cache->entries[i].valid = 0;
    }

    cache->capacity = capacity;
    cache->sector_size = sector_size;
    cache->clock = 0;
    cache->media = media;

    return ODFS_OK;
}

void odfs_cache_destroy(odfs_cache_t *cache)
{
    if (!cache || !cache->entries)
        return;

    for (uint32_t i = 0; i < cache->capacity; i++)
        odfs_free(cache->entries[i].data);
    odfs_free(cache->entries);

    memset(cache, 0, sizeof(*cache));
}

void odfs_cache_flush(odfs_cache_t *cache)
{
    if (!cache || !cache->entries)
        return;

    for (uint32_t i = 0; i < cache->capacity; i++)
        cache->entries[i].valid = 0;
}

odfs_err_t odfs_cache_read(odfs_cache_t *cache,
                             uint32_t lba,
                             const uint8_t **out)
{
    uint32_t i;
    uint32_t victim = 0;
    uint32_t oldest_age = UINT32_MAX;
    uint32_t used = 0;
    odfs_err_t err;

    if (!cache || !cache->entries || !out)
        return ODFS_ERR_INVAL;

    cache->stats.reads++;
    cache->clock++;

    /* search for hit */
    for (i = 0; i < cache->capacity; i++) {
        if (cache->entries[i].valid) {
            used++;
            if (cache->entries[i].lba == lba) {
                /* hit */
                cache->entries[i].age = cache->clock;
                cache->stats.hits++;
                *out = cache->entries[i].data;
                return ODFS_OK;
            }
        }
    }

    /* miss — find victim (LRU or first invalid) */
    cache->stats.misses++;

    for (i = 0; i < cache->capacity; i++) {
        if (!cache->entries[i].valid) {
            victim = i;
            goto fill;
        }
        if (cache->entries[i].age < oldest_age) {
            oldest_age = cache->entries[i].age;
            victim = i;
        }
    }

    /* evicting a valid entry */
    cache->stats.evictions++;

fill:
    err = odfs_media_read(cache->media, lba, 1, cache->entries[victim].data);
    if (err != ODFS_OK)
        return err;

    cache->entries[victim].lba = lba;
    cache->entries[victim].age = cache->clock;
    cache->entries[victim].valid = 1;

    /* track high-water mark */
    used = 0;
    for (i = 0; i < cache->capacity; i++) {
        if (cache->entries[i].valid)
            used++;
    }
    if (used > cache->stats.max_used)
        cache->stats.max_used = used;

    *out = cache->entries[victim].data;
    return ODFS_OK;
}

const odfs_cache_stats_t *odfs_cache_get_stats(const odfs_cache_t *cache)
{
    return &cache->stats;
}
