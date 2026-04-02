/*
 * test_cache.c — tests for block cache
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "odfs/cache.h"
#include "test_harness.h"

/* mock media: returns sector data = LBA value repeated */
#define MOCK_SECTOR_SIZE 2048
#define MOCK_SECTOR_COUNT 100

static odfs_err_t mock_read_sectors(void *ctx, uint32_t lba,
                                     uint32_t count, void *buf)
{
    int *read_count = ctx;
    uint8_t *out = buf;

    for (uint32_t s = 0; s < count; s++) {
        uint8_t fill = (uint8_t)((lba + s) & 0xFF);
        for (uint32_t i = 0; i < MOCK_SECTOR_SIZE; i++)
            out[s * MOCK_SECTOR_SIZE + i] = fill;
    }

    if (read_count)
        (*read_count) += count;

    return ODFS_OK;
}

static uint32_t mock_sector_size(void *ctx)
{
    (void)ctx;
    return MOCK_SECTOR_SIZE;
}

static uint32_t mock_sector_count(void *ctx)
{
    (void)ctx;
    return MOCK_SECTOR_COUNT;
}

static const odfs_media_ops_t mock_ops = {
    .read_sectors = mock_read_sectors,
    .sector_size  = mock_sector_size,
    .sector_count = mock_sector_count,
    .read_toc     = NULL,
    .close        = NULL,
};

static void make_mock_media(odfs_media_t *m, int *read_count)
{
    m->ops = &mock_ops;
    m->ctx = read_count;
}

TEST(cache_init_destroy)
{
    odfs_cache_t cache;
    odfs_media_t media;
    int reads = 0;

    make_mock_media(&media, &reads);
    ASSERT_OK(odfs_cache_init(&cache, &media, 4));
    ASSERT_EQ(cache.capacity, 4);
    odfs_cache_destroy(&cache);
}

TEST(cache_basic_read)
{
    odfs_cache_t cache;
    odfs_media_t media;
    const uint8_t *data;
    int reads = 0;

    make_mock_media(&media, &reads);
    ASSERT_OK(odfs_cache_init(&cache, &media, 4));

    ASSERT_OK(odfs_cache_read(&cache, 5, &data));
    ASSERT_EQ(data[0], 5);
    ASSERT_EQ(reads, 1);

    odfs_cache_destroy(&cache);
}

TEST(cache_hit)
{
    odfs_cache_t cache;
    odfs_media_t media;
    const uint8_t *data;
    int reads = 0;

    make_mock_media(&media, &reads);
    ASSERT_OK(odfs_cache_init(&cache, &media, 4));

    ASSERT_OK(odfs_cache_read(&cache, 10, &data));
    ASSERT_EQ(reads, 1);
    ASSERT_EQ(data[0], 10);

    /* second read should be a cache hit */
    ASSERT_OK(odfs_cache_read(&cache, 10, &data));
    ASSERT_EQ(reads, 1); /* no additional I/O */
    ASSERT_EQ(data[0], 10);

    const odfs_cache_stats_t *stats = odfs_cache_get_stats(&cache);
    ASSERT_EQ(stats->hits, 1);
    ASSERT_EQ(stats->misses, 1);

    odfs_cache_destroy(&cache);
}

TEST(cache_eviction)
{
    odfs_cache_t cache;
    odfs_media_t media;
    const uint8_t *data;
    int reads = 0;

    make_mock_media(&media, &reads);
    ASSERT_OK(odfs_cache_init(&cache, &media, 2)); /* tiny cache */

    /* fill both slots */
    ASSERT_OK(odfs_cache_read(&cache, 0, &data));
    ASSERT_OK(odfs_cache_read(&cache, 1, &data));
    ASSERT_EQ(reads, 2);

    /* this should evict LBA 0 (oldest) */
    ASSERT_OK(odfs_cache_read(&cache, 2, &data));
    ASSERT_EQ(reads, 3);

    const odfs_cache_stats_t *stats = odfs_cache_get_stats(&cache);
    ASSERT_EQ(stats->evictions, 1);

    /* reading LBA 0 again should be a miss */
    ASSERT_OK(odfs_cache_read(&cache, 0, &data));
    ASSERT_EQ(reads, 4);
    ASSERT_EQ(data[0], 0);

    odfs_cache_destroy(&cache);
}

TEST(cache_flush)
{
    odfs_cache_t cache;
    odfs_media_t media;
    const uint8_t *data;
    int reads = 0;

    make_mock_media(&media, &reads);
    ASSERT_OK(odfs_cache_init(&cache, &media, 4));

    ASSERT_OK(odfs_cache_read(&cache, 5, &data));
    ASSERT_EQ(reads, 1);

    odfs_cache_flush(&cache);

    /* after flush, should miss */
    ASSERT_OK(odfs_cache_read(&cache, 5, &data));
    ASSERT_EQ(reads, 2);

    odfs_cache_destroy(&cache);
}

TEST(cache_stats_tracking)
{
    odfs_cache_t cache;
    odfs_media_t media;
    const uint8_t *data;
    int reads = 0;

    make_mock_media(&media, &reads);
    ASSERT_OK(odfs_cache_init(&cache, &media, 4));

    const odfs_cache_stats_t *stats = odfs_cache_get_stats(&cache);
    ASSERT_EQ(stats->reads, 0);

    ASSERT_OK(odfs_cache_read(&cache, 1, &data));
    ASSERT_OK(odfs_cache_read(&cache, 2, &data));
    ASSERT_OK(odfs_cache_read(&cache, 1, &data)); /* hit */

    ASSERT_EQ(stats->reads, 3);
    ASSERT_EQ(stats->hits, 1);
    ASSERT_EQ(stats->misses, 2);
    ASSERT_EQ(stats->max_used, 2);

    odfs_cache_destroy(&cache);
}

TEST_MAIN()
