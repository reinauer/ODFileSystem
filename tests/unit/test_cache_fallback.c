/*
 * test_cache_fallback.c — block cache without a sector buffer slab
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Builds the block cache with a slab allocator that always fails, so
 * odfs_cache_init() has to allocate one buffer per entry.
 */

#include "odfs/alloc.h"

#define odfs_malloc_fast(size) ((void)(size), (void *)NULL)
#include "../../core/cache_block.c"

#include "test_harness.h"

#define MOCK_SECTOR_SIZE 2048

static odfs_err_t mock_read_sectors(void *ctx, uint32_t lba,
                                    uint32_t count, void *buf)
{
    int *reads = ctx;

    memset(buf, (int)(lba & 0xFF), (size_t)count * MOCK_SECTOR_SIZE);
    (*reads) += (int)count;
    return ODFS_OK;
}

static uint32_t mock_sector_size(void *ctx)
{
    (void)ctx;
    return MOCK_SECTOR_SIZE;
}

static const odfs_media_ops_t mock_ops = {
    .read_sectors = mock_read_sectors,
    .sector_size  = mock_sector_size,
};

TEST(cache_falls_back_to_per_entry_buffers)
{
    odfs_cache_t cache;
    int reads = 0;
    odfs_media_t media = { &mock_ops, &reads };
    const uint8_t *data;
    uint32_t lba;

    ASSERT_OK(odfs_cache_init(&cache, &media, 4));
    ASSERT(cache.data_slab == NULL);
    for (uint32_t i = 0; i < 4; i++) {
        ASSERT(cache.entries[i].data != NULL);
        for (uint32_t j = 0; j < i; j++)
            ASSERT(cache.entries[i].data != cache.entries[j].data);
    }

    for (lba = 20; lba < 26; lba++) {
        ASSERT_OK(odfs_cache_read(&cache, lba, &data));
        ASSERT_EQ(data[0], lba);
        ASSERT_EQ(data[MOCK_SECTOR_SIZE - 1], lba);
    }
    ASSERT_EQ(reads, 6);

    ASSERT_OK(odfs_cache_read(&cache, 25, &data));
    ASSERT_EQ(data[0], 25);
    ASSERT_EQ(reads, 6);

    odfs_cache_destroy(&cache);
    ASSERT(cache.entries == NULL);
}

TEST_MAIN()
