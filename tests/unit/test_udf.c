/*
 * test_udf.c — tests for UDF backend helpers and directory parsing
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "odfs/api.h"
#include "odfs/cache.h"
#include "udf/udf.h"
#include "test_harness.h"

#include <string.h>

#define MOCK_SECTOR_SIZE 2048
#define MOCK_SECTOR_COUNT 8

typedef struct udf_mock_media {
    uint8_t sectors[MOCK_SECTOR_COUNT][MOCK_SECTOR_SIZE];
} udf_mock_media_t;

static odfs_err_t udf_mock_read_sectors(void *ctx, uint32_t lba,
                                        uint32_t count, void *buf)
{
    udf_mock_media_t *media = ctx;

    if (lba + count > MOCK_SECTOR_COUNT)
        return ODFS_ERR_RANGE;

    memcpy(buf, &media->sectors[lba][0], (size_t)count * MOCK_SECTOR_SIZE);
    return ODFS_OK;
}

static uint32_t udf_mock_sector_size(void *ctx)
{
    (void)ctx;
    return MOCK_SECTOR_SIZE;
}

static uint32_t udf_mock_sector_count(void *ctx)
{
    (void)ctx;
    return MOCK_SECTOR_COUNT;
}

static const odfs_media_ops_t udf_mock_ops = {
    .read_sectors = udf_mock_read_sectors,
    .sector_size  = udf_mock_sector_size,
    .sector_count = udf_mock_sector_count,
    .read_toc     = NULL,
    .read_audio   = NULL,
    .close        = NULL,
};

static void udf_make_media(odfs_media_t *media, udf_mock_media_t *ctx)
{
    media->ops = &udf_mock_ops;
    media->ctx = ctx;
}

static void udf_write_tag(uint8_t *buf, uint16_t id)
{
    memset(buf, 0, 16);
    buf[0] = (uint8_t)(id & 0xFF);
    buf[1] = (uint8_t)(id >> 8);
    buf[2] = 2; /* descriptor version */

    uint8_t sum = 0;
    for (int i = 0; i < 16; i++) {
        if (i != 4)
            sum += buf[i];
    }
    buf[4] = sum;
}

static void udf_write_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static void udf_write_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void udf_write_le64(uint8_t *p, uint64_t v)
{
    udf_write_le32(p, (uint32_t)(v & 0xFFFFFFFFu));
    udf_write_le32(p + 4, (uint32_t)(v >> 32));
}

static void udf_make_fe(uint8_t *sector, uint8_t file_type,
                        uint64_t size, uint32_t data_pos)
{
    memset(sector, 0, MOCK_SECTOR_SIZE);
    udf_write_tag(sector, UDF_TAG_FE);
    sector[16 + 11] = file_type;
    udf_write_le16(&sector[16 + 18], UDF_ICB_ALLOC_SHORT);
    udf_write_le64(&sector[56], size);
    udf_write_le32(&sector[168], 0); /* ea_len */
    udf_write_le32(&sector[172], 8); /* ad_length */
    udf_write_le32(&sector[176], (uint32_t)size);
    udf_write_le32(&sector[180], data_pos);
}

typedef struct collect_ctx {
    odfs_node_t node;
    int         count;
} collect_ctx_t;

static odfs_err_t collect_one(const odfs_node_t *entry, void *ctx)
{
    collect_ctx_t *collect = ctx;
    collect->node = *entry;
    collect->count++;
    return ODFS_OK;
}

TEST(udf_readdir_cross_sector_fid)
{
    udf_mock_media_t mock;
    odfs_media_t media;
    odfs_cache_t cache;
    odfs_log_state_t log;
    udf_context_t ctx;
    odfs_node_t dir;
    collect_ctx_t collect;
    odfs_err_t err;
    uint8_t filler[2028];
    uint8_t fid[40];

    memset(&mock, 0, sizeof(mock));
    memset(&ctx, 0, sizeof(ctx));
    memset(&dir, 0, sizeof(dir));
    memset(&collect, 0, sizeof(collect));

    ctx.next_node_id = 1;

    udf_make_media(&media, &mock);
    ASSERT_OK(odfs_cache_init(&cache, &media, 4));
    odfs_log_init(&log);

    /* sector 0: root directory FE, directory data starts at LBA 1 */
    udf_make_fe(mock.sectors[0], UDF_ICB_FILETYPE_DIR, 2068, 1);

    /* sector 3: file FE for the directory entry */
    udf_make_fe(mock.sectors[3], UDF_ICB_FILETYPE_FILE, 5, 4);

    /* filler FID so the real entry begins 20 bytes before the sector end */
    memset(filler, 0, sizeof(filler));
    udf_write_tag(filler, UDF_TAG_FID);
    udf_write_le16(&filler[16], 1);
    filler[18] = UDF_FID_FLAG_DELETED;
    filler[19] = 0;
    udf_write_le32(&filler[24], 0);
    udf_write_le16(&filler[28], 0);
    udf_write_le16(&filler[36], 1990);
    memcpy(&mock.sectors[1][0], filler, sizeof(filler));

    /* directory FID starting 20 bytes before the end of sector 1 */
    memset(fid, 0, sizeof(fid));
    udf_write_tag(fid, UDF_TAG_FID);
    udf_write_le16(&fid[16], 1);  /* fid version */
    fid[18] = 0;                  /* flags */
    fid[19] = 2;                  /* CS0 name length: comp_id + 'A' */
    udf_write_le32(&fid[24], 3);  /* file ICB LBA */
    udf_write_le16(&fid[28], 0);  /* partition */
    udf_write_le16(&fid[36], 0);  /* impl_len */
    fid[38] = 8;                  /* CS0 8-bit */
    fid[39] = 'A';

    memcpy(&mock.sectors[1][2028], fid, 20);
    memcpy(&mock.sectors[2][0], fid + 20, sizeof(fid) - 20);

    dir.id = 0;
    dir.parent_id = 0;
    dir.backend = ODFS_BACKEND_UDF;
    dir.kind = ODFS_NODE_DIR;
    dir.name[0] = '/';
    dir.name[1] = '\0';
    dir.extent.lba = 0; /* root FE sector */

    err = udf_backend_ops.readdir(&ctx, &cache, &log, &dir,
                                  collect_one, &collect, NULL);
    ASSERT_OK(err);
    ASSERT_EQ(collect.count, 1);
    ASSERT_STR_EQ(collect.node.name, "A");
    ASSERT_EQ(collect.node.kind, ODFS_NODE_FILE);
    ASSERT_EQ(collect.node.size, 5);

    odfs_cache_destroy(&cache);
}

TEST_MAIN()
