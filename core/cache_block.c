/*
 * cache_block.c — LRU block cache
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "odfs/cache.h"
#include "odfs/alloc.h"
#include <string.h>

#define ODFS_CACHE_STREAM_MIN_SECTORS 2u

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

        if (full_sectors > UINT32_MAX)
            count = UINT32_MAX;
        else
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
