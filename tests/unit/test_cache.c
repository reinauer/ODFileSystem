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

typedef struct stream_read_counts {
    int calls;
    int sectors;
} stream_read_counts_t;

static odfs_err_t stream_mock_read_sectors(void *ctx, uint32_t lba,
                                           uint32_t count, void *buf)
{
    stream_read_counts_t *reads = ctx;
    uint8_t *out = buf;

    if (reads) {
        reads->calls++;
        reads->sectors += (int)count;
    }

    for (uint32_t s = 0; s < count; s++) {
        uint8_t fill = (uint8_t)((lba + s) & 0xFF);
        for (uint32_t i = 0; i < MOCK_SECTOR_SIZE; i++)
            out[s * MOCK_SECTOR_SIZE + i] = fill;
    }

    return ODFS_OK;
}

static const odfs_media_ops_t stream_mock_ops = {
    .read_sectors = stream_mock_read_sectors,
    .sector_size  = mock_sector_size,
    .sector_count = mock_sector_count,
    .read_toc     = NULL,
    .close        = NULL,
};

static void make_stream_media(odfs_media_t *m, stream_read_counts_t *reads)
{
    m->ops = &stream_mock_ops;
    m->ctx = reads;
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

TEST(cache_hit_updates_lru_order)
{
    odfs_cache_t cache;
    odfs_media_t media;
    const uint8_t *data;
    int reads = 0;

    make_mock_media(&media, &reads);
    ASSERT_OK(odfs_cache_init(&cache, &media, 2));

    ASSERT_OK(odfs_cache_read(&cache, 0, &data));
    ASSERT_OK(odfs_cache_read(&cache, 1, &data));
    ASSERT_OK(odfs_cache_read(&cache, 0, &data));
    ASSERT_EQ(reads, 2);

    ASSERT_OK(odfs_cache_read(&cache, 2, &data));
    ASSERT_EQ(reads, 3);

    ASSERT_OK(odfs_cache_read(&cache, 0, &data));
    ASSERT_EQ(reads, 3);

    ASSERT_OK(odfs_cache_read(&cache, 1, &data));
    ASSERT_EQ(reads, 4);

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

TEST(cache_read_bytes_batches_aligned_runs)
{
    odfs_cache_t cache;
    odfs_media_t media;
    stream_read_counts_t reads = {0, 0};
    uint8_t buf[MOCK_SECTOR_SIZE * 6];
    size_t len = sizeof(buf);

    make_stream_media(&media, &reads);
    ASSERT_OK(odfs_cache_init(&cache, &media, 4));

    ASSERT_OK(odfs_cache_read_bytes(&cache, 10, 0, buf, &len));
    ASSERT_EQ(len, sizeof(buf));
    ASSERT_EQ(reads.calls, 1);
    ASSERT_EQ(reads.sectors, 6);
    ASSERT_EQ(buf[0], 10);
    ASSERT_EQ(buf[MOCK_SECTOR_SIZE * 5], 15);

    odfs_cache_destroy(&cache);
}

TEST(cache_read_bytes_caches_unaligned_edges)
{
    odfs_cache_t cache;
    odfs_media_t media;
    stream_read_counts_t reads = {0, 0};
    uint8_t buf[MOCK_SECTOR_SIZE * 3];
    size_t len = sizeof(buf);

    make_stream_media(&media, &reads);
    ASSERT_OK(odfs_cache_init(&cache, &media, 4));

    ASSERT_OK(odfs_cache_read_bytes(&cache, 20, 100, buf, &len));
    ASSERT_EQ(len, sizeof(buf));
    ASSERT_EQ(reads.calls, 3);
    ASSERT_EQ(reads.sectors, 4);
    ASSERT_EQ(buf[0], 20);
    ASSERT_EQ(buf[MOCK_SECTOR_SIZE - 100], 21);
    ASSERT_EQ(buf[len - 1], 23);

    odfs_cache_destroy(&cache);
}

TEST_MAIN()
