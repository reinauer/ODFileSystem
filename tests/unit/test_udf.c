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
    int     read_counts[MOCK_SECTOR_COUNT];
    int     fail_io_lba;
} udf_mock_media_t;

static odfs_err_t udf_mock_read_sectors(void *ctx, uint32_t lba,
                                        uint32_t count, void *buf)
{
    udf_mock_media_t *media = ctx;

    if (lba + count > MOCK_SECTOR_COUNT)
        return ODFS_ERR_RANGE;

    for (uint32_t i = 0; i < count; i++) {
        if ((int)(lba + i) == media->fail_io_lba)
            return ODFS_ERR_IO;
        media->read_counts[lba + i]++;
    }

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
    ctx->fail_io_lba = -1;
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

static odfs_err_t collect_one_and_stop(const odfs_node_t *entry, void *ctx)
{
    collect_one(entry, ctx);
    return ODFS_ERR_EOF;
}

static void udf_make_fid(uint8_t *fid, const char *name, uint32_t icb_lba)
{
    size_t name_len = strlen(name);

    memset(fid, 0, 40);
    udf_write_tag(fid, UDF_TAG_FID);
    udf_write_le16(&fid[16], 1);  /* fid version */
    fid[18] = 0;                  /* flags */
    fid[19] = (uint8_t)(name_len + 1u);
    udf_write_le32(&fid[24], icb_lba);
    udf_write_le16(&fid[28], 0);  /* partition */
    udf_write_le16(&fid[36], 0);  /* impl_len */
    fid[38] = 8;                  /* CS0 8-bit */
    memcpy(&fid[39], name, name_len);
}

static void udf_make_parent_fid(uint8_t *fid, uint32_t parent_icb_lba)
{
    memset(fid, 0, 40);
    udf_write_tag(fid, UDF_TAG_FID);
    udf_write_le16(&fid[16], 1);  /* fid version */
    fid[18] = UDF_FID_FLAG_PARENT;
    fid[19] = 0;
    udf_write_le32(&fid[24], parent_icb_lba);
    udf_write_le16(&fid[28], 0);  /* partition */
    udf_write_le16(&fid[36], 0);  /* impl_len */
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

TEST(udf_readdir_resume_skips_prior_icb_reads)
{
    udf_mock_media_t mock;
    odfs_media_t media;
    odfs_cache_t cache;
    odfs_log_state_t log;
    udf_context_t ctx;
    odfs_node_t dir;
    collect_ctx_t collect;
    uint32_t resume = 0;
    uint8_t fid[40];
    odfs_err_t err;

    memset(&mock, 0, sizeof(mock));
    memset(&ctx, 0, sizeof(ctx));
    memset(&dir, 0, sizeof(dir));
    memset(&collect, 0, sizeof(collect));

    udf_make_media(&media, &mock);
    ASSERT_OK(odfs_cache_init(&cache, &media, 4));
    odfs_log_init(&log);

    udf_make_fe(mock.sectors[0], UDF_ICB_FILETYPE_DIR, 80, 1);
    udf_make_fe(mock.sectors[3], UDF_ICB_FILETYPE_FILE, 5, 5);
    udf_make_fe(mock.sectors[4], UDF_ICB_FILETYPE_FILE, 7, 6);

    udf_make_fid(fid, "A", 3);
    memcpy(&mock.sectors[1][0], fid, sizeof(fid));
    udf_make_fid(fid, "B", 4);
    memcpy(&mock.sectors[1][sizeof(fid)], fid, sizeof(fid));

    dir.id = 0;
    dir.parent_id = 0;
    dir.backend = ODFS_BACKEND_UDF;
    dir.kind = ODFS_NODE_DIR;
    dir.name[0] = '/';
    dir.name[1] = '\0';
    dir.extent.lba = 0;

    err = udf_backend_ops.readdir(&ctx, &cache, &log, &dir,
                                  collect_one_and_stop, &collect, &resume);
    ASSERT_EQ(err, ODFS_ERR_EOF);
    ASSERT_EQ(collect.count, 1);
    ASSERT_STR_EQ(collect.node.name, "A");
    ASSERT(resume > 0);

    memset(&collect, 0, sizeof(collect));
    memset(mock.read_counts, 0, sizeof(mock.read_counts));
    odfs_cache_flush(&cache);

    err = udf_backend_ops.readdir(&ctx, &cache, &log, &dir,
                                  collect_one, &collect, &resume);
    ASSERT_OK(err);
    ASSERT_EQ(collect.count, 1);
    ASSERT_STR_EQ(collect.node.name, "B");
    ASSERT_EQ(collect.node.size, 7);
    ASSERT_EQ(mock.read_counts[3], 0);
    ASSERT(mock.read_counts[4] > 0);

    odfs_cache_destroy(&cache);
}

TEST(udf_lookup_skips_nonmatching_icb_reads)
{
    udf_mock_media_t mock;
    odfs_media_t media;
    odfs_cache_t cache;
    odfs_log_state_t log;
    udf_context_t ctx;
    odfs_node_t dir;
    odfs_node_t out;
    uint8_t fid[40];

    memset(&mock, 0, sizeof(mock));
    memset(&ctx, 0, sizeof(ctx));
    memset(&dir, 0, sizeof(dir));
    memset(&out, 0, sizeof(out));

    udf_make_media(&media, &mock);
    ASSERT_OK(odfs_cache_init(&cache, &media, 4));
    odfs_log_init(&log);

    udf_make_fe(mock.sectors[0], UDF_ICB_FILETYPE_DIR, 80, 1);
    udf_make_fe(mock.sectors[3], UDF_ICB_FILETYPE_FILE, 5, 5);
    udf_make_fe(mock.sectors[4], UDF_ICB_FILETYPE_FILE, 7, 6);

    udf_make_fid(fid, "A", 3);
    memcpy(&mock.sectors[1][0], fid, sizeof(fid));
    udf_make_fid(fid, "B", 4);
    memcpy(&mock.sectors[1][sizeof(fid)], fid, sizeof(fid));

    dir.id = 0;
    dir.parent_id = 0;
    dir.backend = ODFS_BACKEND_UDF;
    dir.kind = ODFS_NODE_DIR;
    dir.name[0] = '/';
    dir.name[1] = '\0';
    dir.extent.lba = 0;

    ASSERT_OK(udf_backend_ops.lookup(&ctx, &cache, &log, &dir, "B", &out));
    ASSERT_STR_EQ(out.name, "B");
    ASSERT_EQ(out.size, 7);
    ASSERT_EQ(mock.read_counts[3], 0);
    ASSERT(mock.read_counts[4] > 0);

    odfs_cache_destroy(&cache);
}

TEST(udf_lookup_propagates_truncated_directory)
{
    udf_mock_media_t mock;
    odfs_media_t media;
    odfs_cache_t cache;
    odfs_log_state_t log;
    udf_context_t ctx;
    odfs_node_t dir;
    odfs_node_t out;
    uint8_t fid[40];

    memset(&mock, 0, sizeof(mock));
    memset(&ctx, 0, sizeof(ctx));
    memset(&dir, 0, sizeof(dir));
    memset(&out, 0, sizeof(out));

    udf_make_media(&media, &mock);
    ASSERT_OK(odfs_cache_init(&cache, &media, 4));
    odfs_log_init(&log);

    udf_make_fe(mock.sectors[0], UDF_ICB_FILETYPE_DIR, 80, 1);
    udf_write_le32(&mock.sectors[0][176], 40);
    udf_make_fid(fid, "A", 3);
    memcpy(&mock.sectors[1][0], fid, sizeof(fid));

    dir.backend = ODFS_BACKEND_UDF;
    dir.kind = ODFS_NODE_DIR;
    dir.extent.lba = 0;

    ASSERT_ERR(udf_backend_ops.lookup(&ctx, &cache, &log, &dir,
                                      "B", &out),
               ODFS_ERR_EOF);

    odfs_cache_destroy(&cache);
}

TEST(udf_resolve_parent_skips_nonmatching_child_icb_reads)
{
    udf_mock_media_t mock;
    odfs_media_t media;
    odfs_cache_t cache;
    odfs_log_state_t log;
    udf_context_t ctx;
    odfs_node_t child;
    odfs_node_t parent;
    uint8_t fid[40];

    memset(&mock, 0, sizeof(mock));
    memset(&ctx, 0, sizeof(ctx));
    memset(&child, 0, sizeof(child));
    memset(&parent, 0, sizeof(parent));

    ctx.root.id = 0;
    ctx.root.parent_id = 0;
    ctx.root.backend = ODFS_BACKEND_UDF;
    ctx.root.kind = ODFS_NODE_DIR;
    ctx.root.name[0] = '/';
    ctx.root.name[1] = '\0';
    ctx.root.extent.lba = 0;

    udf_make_media(&media, &mock);
    ASSERT_OK(odfs_cache_init(&cache, &media, 4));
    odfs_log_init(&log);

    udf_make_fe(mock.sectors[0], UDF_ICB_FILETYPE_DIR, 80, 1);
    udf_make_fe(mock.sectors[3], UDF_ICB_FILETYPE_DIR, 40, 6);
    udf_make_fe(mock.sectors[4], UDF_ICB_FILETYPE_DIR, 40, 2);
    udf_make_fe(mock.sectors[5], UDF_ICB_FILETYPE_DIR, 40, 7);

    udf_make_fid(fid, "A", 5);
    memcpy(&mock.sectors[1][0], fid, sizeof(fid));
    udf_make_fid(fid, "P", 3);
    fid[18] = UDF_FID_FLAG_DIRECTORY;
    memcpy(&mock.sectors[1][sizeof(fid)], fid, sizeof(fid));

    udf_make_parent_fid(fid, 3);
    memcpy(&mock.sectors[2][0], fid, sizeof(fid));
    udf_make_parent_fid(fid, 0);
    memcpy(&mock.sectors[6][0], fid, sizeof(fid));

    child.id = 42;
    child.parent_id = 0;
    child.backend = ODFS_BACKEND_UDF;
    child.kind = ODFS_NODE_DIR;
    child.name[0] = 'C';
    child.name[1] = '\0';
    child.extent.lba = 4;

    ASSERT_OK(udf_backend_ops.resolve_parent(&ctx, &cache, &log,
                                             &child, &parent, NULL));
    ASSERT_STR_EQ(parent.name, "P");
    ASSERT_EQ(parent.extent.lba, 3);
    ASSERT_EQ(mock.read_counts[5], 0);
    ASSERT(mock.read_counts[3] > 0);

    odfs_cache_destroy(&cache);
}

/* ---- multi-extent read path ---- */

/*
 * Write an FE whose data is described by several short allocation
 * descriptors. Each AD is a (length-with-type, position) pair; the
 * caller encodes the extent type in the top bits of the length.
 */
static void udf_make_fe_ads(uint8_t *sector, uint8_t file_type, uint64_t size,
                            const uint32_t *ad_pairs, int n_ads)
{
    memset(sector, 0, MOCK_SECTOR_SIZE);
    udf_write_tag(sector, UDF_TAG_FE);
    sector[16 + 11] = file_type;
    udf_write_le16(&sector[16 + 18], UDF_ICB_ALLOC_SHORT);
    udf_write_le64(&sector[56], size);
    udf_write_le32(&sector[168], 0);                    /* ea_len */
    udf_write_le32(&sector[172], (uint32_t)(n_ads * 8)); /* ad_length */
    for (int i = 0; i < n_ads; i++) {
        udf_write_le32(&sector[176 + i * 8], ad_pairs[i * 2]);
        udf_write_le32(&sector[180 + i * 8], ad_pairs[i * 2 + 1]);
    }
}

static void udf_setup_read_env(udf_mock_media_t *mock, odfs_media_t *media,
                               odfs_cache_t *cache, odfs_log_state_t *log,
                               udf_context_t *ctx, odfs_node_t *file,
                               uint64_t size)
{
    memset(mock, 0, sizeof(*mock));
    memset(ctx, 0, sizeof(*ctx));
    memset(file, 0, sizeof(*file));

    udf_make_media(media, mock);
    odfs_cache_init(cache, media, 4);
    odfs_log_init(log);

    file->backend = ODFS_BACKEND_UDF;
    file->kind = ODFS_NODE_FILE;
    file->extent.lba = 0; /* FE sector */
    file->size = size;
}

TEST(udf_read_multi_extent_file)
{
    udf_mock_media_t mock;
    odfs_media_t media;
    odfs_cache_t cache;
    odfs_log_state_t log;
    udf_context_t ctx;
    odfs_node_t file;
    uint8_t buf[64];
    size_t len;
    int i;

    /* two discontiguous extents: 2048 bytes at LBA 2, then 5 at LBA 1 */
    static const uint32_t ads[] = { 2048, 2, 5, 1 };

    udf_setup_read_env(&mock, &media, &cache, &log, &ctx, &file, 2053);
    udf_make_fe_ads(mock.sectors[0], UDF_ICB_FILETYPE_FILE, 2053, ads, 2);
    memset(mock.sectors[2], 'A', MOCK_SECTOR_SIZE);
    memset(mock.sectors[1], 'B', 5);

    /* read across the extent boundary */
    memset(buf, 0, sizeof(buf));
    len = 13;
    ASSERT_OK(udf_backend_ops.read(&ctx, &cache, &log, &file,
                                   2040, buf, &len));
    ASSERT_EQ(len, 13);
    for (i = 0; i < 8; i++)
        ASSERT_EQ(buf[i], 'A');
    for (i = 8; i < 13; i++)
        ASSERT_EQ(buf[i], 'B');

    /* a read past EOF is clamped */
    len = sizeof(buf);
    ASSERT_OK(udf_backend_ops.read(&ctx, &cache, &log, &file,
                                   2050, buf, &len));
    ASSERT_EQ(len, 3);

    odfs_cache_destroy(&cache);
}

TEST(udf_read_sparse_extent_reads_zeros)
{
    udf_mock_media_t mock;
    odfs_media_t media;
    odfs_cache_t cache;
    odfs_log_state_t log;
    udf_context_t ctx;
    odfs_node_t file;
    uint8_t buf[32];
    size_t len;
    int i;

    /* recorded, then allocated-but-unrecorded (reads as zeros), then
     * recorded again */
    static const uint32_t ads[] = {
        2048, 1,
        (1u << 30) | 2048, 0,
        5, 2,
    };

    udf_setup_read_env(&mock, &media, &cache, &log, &ctx, &file, 4101);
    udf_make_fe_ads(mock.sectors[0], UDF_ICB_FILETYPE_FILE, 4101, ads, 3);
    memset(mock.sectors[1], 'A', MOCK_SECTOR_SIZE);
    memset(mock.sectors[2], 'C', 5);

    /* straddle the recorded → sparse boundary */
    memset(buf, 0xEE, sizeof(buf));
    len = 8;
    ASSERT_OK(udf_backend_ops.read(&ctx, &cache, &log, &file,
                                   2044, buf, &len));
    ASSERT_EQ(len, 8);
    for (i = 0; i < 4; i++)
        ASSERT_EQ(buf[i], 'A');
    for (i = 4; i < 8; i++)
        ASSERT_EQ(buf[i], 0);

    /* straddle the sparse → recorded boundary */
    memset(buf, 0xEE, sizeof(buf));
    len = 9;
    ASSERT_OK(udf_backend_ops.read(&ctx, &cache, &log, &file,
                                   4092, buf, &len));
    ASSERT_EQ(len, 9);
    for (i = 0; i < 4; i++)
        ASSERT_EQ(buf[i], 0);
    for (i = 4; i < 9; i++)
        ASSERT_EQ(buf[i], 'C');

    odfs_cache_destroy(&cache);
}

TEST(udf_read_unallocated_tail_reads_zeros)
{
    udf_mock_media_t mock;
    odfs_media_t media;
    odfs_cache_t cache;
    odfs_log_state_t log;
    udf_context_t ctx;
    odfs_node_t file;
    uint8_t buf[16];
    size_t len;
    int i;

    /* one recorded 5-byte extent, Information Length 105: the last
     * 100 bytes have no allocation descriptor */
    static const uint32_t ads[] = { 5, 1 };

    udf_setup_read_env(&mock, &media, &cache, &log, &ctx, &file, 105);
    udf_make_fe_ads(mock.sectors[0], UDF_ICB_FILETYPE_FILE, 105, ads, 1);
    memset(mock.sectors[1], 'C', MOCK_SECTOR_SIZE);

    /* straddle the recorded → unallocated tail boundary */
    memset(buf, 0xEE, sizeof(buf));
    len = sizeof(buf);
    ASSERT_OK(udf_backend_ops.read(&ctx, &cache, &log, &file,
                                   0, buf, &len));
    ASSERT_EQ(len, sizeof(buf));
    for (i = 0; i < 5; i++)
        ASSERT_EQ(buf[i], 'C');
    for (i = 5; i < 16; i++)
        ASSERT_EQ(buf[i], 0);

    /* entirely inside the tail, clamped at the Information Length */
    memset(buf, 0xEE, sizeof(buf));
    len = sizeof(buf);
    ASSERT_OK(udf_backend_ops.read(&ctx, &cache, &log, &file,
                                   95, buf, &len));
    ASSERT_EQ(len, 10);
    for (i = 0; i < 10; i++)
        ASSERT_EQ(buf[i], 0);

    odfs_cache_destroy(&cache);
}

TEST(udf_read_follows_aed_continuation)
{
    udf_mock_media_t mock;
    odfs_media_t media;
    odfs_cache_t cache;
    odfs_log_state_t log;
    udf_context_t ctx;
    odfs_node_t file;
    uint8_t buf[16];
    size_t len;
    int i;

    /* FE holds one data extent and a type-3 continuation pointing at
     * an AED in sector 3, which holds the final extent */
    static const uint32_t fe_ads[] = {
        2048, 1,
        (3u << 30) | 2048, 3,
    };

    udf_setup_read_env(&mock, &media, &cache, &log, &ctx, &file, 2053);
    udf_make_fe_ads(mock.sectors[0], UDF_ICB_FILETYPE_FILE, 2053, fe_ads, 2);
    memset(mock.sectors[1], 'A', MOCK_SECTOR_SIZE);
    memset(mock.sectors[2], 'B', 5);

    /* AED: tag + previous-extent(4) + ad-length(4), ADs at offset 24 */
    udf_write_tag(mock.sectors[3], UDF_TAG_AED);
    udf_write_le32(&mock.sectors[3][20], 8);
    udf_write_le32(&mock.sectors[3][24], 5);
    udf_write_le32(&mock.sectors[3][28], 2);

    memset(buf, 0, sizeof(buf));
    len = 13;
    ASSERT_OK(udf_backend_ops.read(&ctx, &cache, &log, &file,
                                   2040, buf, &len));
    ASSERT_EQ(len, 13);
    for (i = 0; i < 8; i++)
        ASSERT_EQ(buf[i], 'A');
    for (i = 8; i < 13; i++)
        ASSERT_EQ(buf[i], 'B');

    odfs_cache_destroy(&cache);
}

TEST(udf_read_follows_many_descriptors)
{
    udf_mock_media_t mock;
    odfs_media_t media;
    odfs_cache_t cache;
    odfs_log_state_t log;
    udf_context_t ctx;
    odfs_node_t file;
    uint32_t ads[2 * 12];
    uint8_t buf[12 * 100];
    size_t len;
    int i;

    /* twelve 100-byte extents, alternating between sectors 1 and 2,
     * more descriptors than fit in one window of the cursor */
    for (i = 0; i < 12; i++) {
        ads[i * 2] = 100;
        ads[i * 2 + 1] = 1u + (uint32_t)(i & 1);
    }

    udf_setup_read_env(&mock, &media, &cache, &log, &ctx, &file, 1200);
    udf_make_fe_ads(mock.sectors[0], UDF_ICB_FILETYPE_FILE, 1200, ads, 12);
    memset(mock.sectors[1], 'A', MOCK_SECTOR_SIZE);
    memset(mock.sectors[2], 'B', MOCK_SECTOR_SIZE);

    len = sizeof(buf);
    ASSERT_OK(udf_backend_ops.read(&ctx, &cache, &log, &file,
                                   0, buf, &len));
    ASSERT_EQ(len, sizeof(buf));
    for (i = 0; i < 1200; i++)
        ASSERT_EQ(buf[i], (i / 100) & 1 ? 'B' : 'A');

    odfs_cache_destroy(&cache);
}

TEST(udf_readdir_multi_extent_directory)
{
    udf_mock_media_t mock;
    odfs_media_t media;
    odfs_cache_t cache;
    odfs_log_state_t log;
    udf_context_t ctx;
    odfs_node_t dir;
    collect_ctx_t collect;
    uint8_t fid[40];
    uint8_t filler[2008];

    /* FID stream spans two discontiguous extents: sector 1 then sector 4 */
    static const uint32_t dir_ads[] = { 2048, 1, 40, 4 };

    memset(&mock, 0, sizeof(mock));
    memset(&ctx, 0, sizeof(ctx));
    memset(&dir, 0, sizeof(dir));
    memset(&collect, 0, sizeof(collect));

    udf_make_media(&media, &mock);
    ASSERT_OK(odfs_cache_init(&cache, &media, 4));
    odfs_log_init(&log);

    udf_make_fe_ads(mock.sectors[0], UDF_ICB_FILETYPE_DIR, 2088, dir_ads, 2);

    /* extent 1: FID for "A", then a deleted filler FID padding the sector */
    udf_make_fid(fid, "A", 5);
    memcpy(&mock.sectors[1][0], fid, 40);

    memset(filler, 0, sizeof(filler));
    udf_write_tag(filler, UDF_TAG_FID);
    udf_write_le16(&filler[16], 1);
    filler[18] = UDF_FID_FLAG_DELETED;
    udf_write_le16(&filler[36], 1970); /* impl_len pads FID to 2008 bytes */
    memcpy(&mock.sectors[1][40], filler, sizeof(filler));

    /* extent 2: FID for "B" */
    udf_make_fid(fid, "B", 5);
    memcpy(&mock.sectors[4][0], fid, 40);

    /* child FE both entries point at */
    udf_make_fe(mock.sectors[5], UDF_ICB_FILETYPE_FILE, 7, 6);

    dir.backend = ODFS_BACKEND_UDF;
    dir.kind = ODFS_NODE_DIR;
    dir.extent.lba = 0;
    dir.extent.length = 2088;
    dir.size = 2088;

    ASSERT_OK(udf_backend_ops.readdir(&ctx, &cache, &log, &dir,
                                      collect_one, &collect, NULL));
    ASSERT_EQ(collect.count, 2);
    ASSERT_STR_EQ(collect.node.name, "B"); /* last collected */
    ASSERT_EQ(collect.node.size, 7);

    odfs_cache_destroy(&cache);
}

/*
 * Directory in sector 0 with FIDs in sector 1: "A" -> FE in sector 3
 * (a corrupt FE that must be skipped), "B" -> FE in sector 4.
 */
static void udf_setup_two_entry_dir(udf_mock_media_t *mock,
                                    odfs_media_t *media,
                                    odfs_cache_t *cache,
                                    odfs_log_state_t *log,
                                    udf_context_t *ctx,
                                    odfs_node_t *dir)
{
    uint8_t fid[40];

    memset(mock, 0, sizeof(*mock));
    memset(ctx, 0, sizeof(*ctx));
    memset(dir, 0, sizeof(*dir));

    udf_make_media(media, mock);
    odfs_cache_init(cache, media, 4);
    odfs_log_init(log);

    udf_make_fe(mock->sectors[0], UDF_ICB_FILETYPE_DIR, 80, 1);
    udf_make_fe(mock->sectors[3], UDF_ICB_FILETYPE_FILE, 5, 5);
    mock->sectors[3][4] ^= 0xFF; /* break the tag checksum */
    udf_make_fe(mock->sectors[4], UDF_ICB_FILETYPE_FILE, 7, 6);

    udf_make_fid(fid, "A", 3);
    memcpy(&mock->sectors[1][0], fid, sizeof(fid));
    udf_make_fid(fid, "B", 4);
    memcpy(&mock->sectors[1][sizeof(fid)], fid, sizeof(fid));

    dir->backend = ODFS_BACKEND_UDF;
    dir->kind = ODFS_NODE_DIR;
    dir->extent.lba = 0;
}

TEST(udf_readdir_propagates_child_fe_io_error)
{
    udf_mock_media_t mock;
    odfs_media_t media;
    odfs_cache_t cache;
    odfs_log_state_t log;
    udf_context_t ctx;
    odfs_node_t dir;
    collect_ctx_t collect;

    udf_setup_two_entry_dir(&mock, &media, &cache, &log, &ctx, &dir);

    /* a corrupt child FE is skipped */
    memset(&collect, 0, sizeof(collect));
    ASSERT_OK(udf_backend_ops.readdir(&ctx, &cache, &log, &dir,
                                      collect_one, &collect, NULL));
    ASSERT_EQ(collect.count, 1);
    ASSERT_STR_EQ(collect.node.name, "B");

    /* a media error on a child FE is not */
    odfs_cache_flush(&cache);
    mock.fail_io_lba = 4;
    memset(&collect, 0, sizeof(collect));
    ASSERT_ERR(udf_backend_ops.readdir(&ctx, &cache, &log, &dir,
                                       collect_one, &collect, NULL),
               ODFS_ERR_IO);
    ASSERT_EQ(collect.count, 0);

    odfs_cache_destroy(&cache);
}

TEST(udf_lookup_propagates_child_fe_io_error)
{
    udf_mock_media_t mock;
    odfs_media_t media;
    odfs_cache_t cache;
    odfs_log_state_t log;
    udf_context_t ctx;
    odfs_node_t dir;
    odfs_node_t out;

    udf_setup_two_entry_dir(&mock, &media, &cache, &log, &ctx, &dir);

    ASSERT_ERR(udf_backend_ops.lookup(&ctx, &cache, &log, &dir, "A", &out),
               ODFS_ERR_NOT_FOUND);

    mock.fail_io_lba = 4;
    ASSERT_ERR(udf_backend_ops.lookup(&ctx, &cache, &log, &dir, "B", &out),
               ODFS_ERR_IO);

    odfs_cache_destroy(&cache);
}

TEST(udf_readlink_current_directory_component)
{
    udf_mock_media_t mock;
    odfs_media_t media;
    odfs_cache_t cache;
    odfs_log_state_t log;
    udf_context_t ctx;
    odfs_node_t dir;
    char target[32];
    uint8_t fid[44];
    uint8_t *fe;

    memset(&mock, 0, sizeof(mock));
    memset(&ctx, 0, sizeof(ctx));
    memset(&dir, 0, sizeof(dir));

    udf_make_media(&media, &mock);
    ASSERT_OK(odfs_cache_init(&cache, &media, 4));
    odfs_log_init(&log);

    udf_make_fe(mock.sectors[0], UDF_ICB_FILETYPE_DIR, sizeof(fid), 1);
    memset(fid, 0, sizeof(fid));
    udf_make_fid(fid, "dot", 3);
    memcpy(&mock.sectors[1][0], fid, sizeof(fid));

    /* symlink with embedded data: one path component of type 4 (".") */
    fe = mock.sectors[3];
    udf_write_tag(fe, UDF_TAG_FE);
    fe[16 + 11] = UDF_ICB_FILETYPE_SYMLINK;
    udf_write_le16(&fe[16 + 18], UDF_ICB_ALLOC_EMBEDDED);
    udf_write_le64(&fe[56], 4);
    udf_write_le32(&fe[172], 4); /* ad_length = embedded data length */
    fe[176] = 4;

    dir.backend = ODFS_BACKEND_UDF;
    dir.kind = ODFS_NODE_DIR;
    dir.extent.lba = 0;

    ASSERT_OK(udf_backend_ops.readlink(&ctx, &cache, &log, &dir, "dot",
                                       target, sizeof(target)));
    ASSERT_STR_EQ(target, ".");

    odfs_cache_destroy(&cache);
}

TEST(udf_node_id_is_stable_across_walks)
{
    udf_mock_media_t mock;
    odfs_media_t media;
    odfs_cache_t cache;
    odfs_log_state_t log;
    udf_context_t ctx;
    odfs_node_t dir;
    odfs_node_t first, again;
    collect_ctx_t collect;

    udf_setup_two_entry_dir(&mock, &media, &cache, &log, &ctx, &dir);

    ASSERT_OK(udf_backend_ops.lookup(&ctx, &cache, &log, &dir, "B", &first));
    ASSERT_OK(udf_backend_ops.lookup(&ctx, &cache, &log, &dir, "B", &again));
    ASSERT_EQ(again.id, first.id);

    memset(&collect, 0, sizeof(collect));
    ASSERT_OK(udf_backend_ops.readdir(&ctx, &cache, &log, &dir,
                                      collect_one, &collect, NULL));
    ASSERT_STR_EQ(collect.node.name, "B");
    ASSERT_EQ(collect.node.id, first.id);
    ASSERT(first.id != 0);

    odfs_cache_destroy(&cache);
}

TEST(udf_readdir_lists_empty_file_without_descriptors)
{
    udf_mock_media_t mock;
    odfs_media_t media;
    odfs_cache_t cache;
    odfs_log_state_t log;
    udf_context_t ctx;
    odfs_node_t dir;
    collect_ctx_t collect;
    uint8_t fid[40];
    size_t len;

    memset(&mock, 0, sizeof(mock));
    memset(&ctx, 0, sizeof(ctx));
    memset(&dir, 0, sizeof(dir));
    memset(&collect, 0, sizeof(collect));

    udf_make_media(&media, &mock);
    ASSERT_OK(odfs_cache_init(&cache, &media, 4));
    odfs_log_init(&log);

    udf_make_fe(mock.sectors[0], UDF_ICB_FILETYPE_DIR, 40, 1);
    udf_make_fid(fid, "E", 3);
    memcpy(&mock.sectors[1][0], fid, sizeof(fid));

    /* zero-length file: short ADs, but no descriptor at all */
    udf_make_fe_ads(mock.sectors[3], UDF_ICB_FILETYPE_FILE, 0, NULL, 0);

    dir.backend = ODFS_BACKEND_UDF;
    dir.kind = ODFS_NODE_DIR;
    dir.extent.lba = 0;

    ASSERT_OK(udf_backend_ops.readdir(&ctx, &cache, &log, &dir,
                                      collect_one, &collect, NULL));
    ASSERT_EQ(collect.count, 1);
    ASSERT_STR_EQ(collect.node.name, "E");
    ASSERT_EQ(collect.node.size, 0);

    len = 16;
    ASSERT_OK(udf_backend_ops.read(&ctx, &cache, &log, &collect.node,
                                   0, fid, &len));
    ASSERT_EQ(len, 0);

    odfs_cache_destroy(&cache);
}

/* deleted FID of the given total length (a multiple of 4, >= 40) */
static void udf_make_deleted_fid(uint8_t *fid, uint32_t length)
{
    memset(fid, 0, 38);
    udf_write_tag(fid, UDF_TAG_FID);
    udf_write_le16(&fid[16], 1);
    fid[18] = UDF_FID_FLAG_DELETED;
    udf_write_le16(&fid[36], (uint16_t)(length - 38u)); /* impl_len */
}

TEST(udf_readdir_name_straddles_sector_after_impl_use)
{
    udf_mock_media_t mock;
    odfs_media_t media;
    odfs_cache_t cache;
    odfs_log_state_t log;
    udf_context_t ctx;
    odfs_node_t dir;
    collect_ctx_t collect;
    uint8_t fid[60];

    memset(&mock, 0, sizeof(mock));
    memset(&ctx, 0, sizeof(ctx));
    memset(&dir, 0, sizeof(dir));
    memset(&collect, 0, sizeof(collect));

    udf_make_media(&media, &mock);
    ASSERT_OK(odfs_cache_init(&cache, &media, 4));
    odfs_log_init(&log);

    /* a deleted FID up to 8 bytes before the end of sector 1, then a
     * 60-byte FID with 8 bytes of implementation use whose header and
     * name continue in sector 2 */
    udf_make_fe(mock.sectors[0], UDF_ICB_FILETYPE_DIR, 2040 + 60, 1);
    udf_make_deleted_fid(mock.sectors[1], 2040);

    memset(fid, 0, sizeof(fid));
    udf_write_tag(fid, UDF_TAG_FID);
    udf_write_le16(&fid[16], 1);
    fid[19] = 11;                 /* CS0 name length */
    udf_write_le32(&fid[24], 3);
    udf_write_le16(&fid[36], 8);  /* impl_len */
    memset(&fid[38], 0xAA, 8);
    fid[46] = 8;
    memcpy(&fid[47], "LONGNAME10", 10);
    memcpy(&mock.sectors[1][2040], fid, 8);
    memcpy(&mock.sectors[2][0], fid + 8, 52);

    udf_make_fe(mock.sectors[3], UDF_ICB_FILETYPE_FILE, 5, 4);

    dir.backend = ODFS_BACKEND_UDF;
    dir.kind = ODFS_NODE_DIR;
    dir.extent.lba = 0;

    ASSERT_OK(udf_backend_ops.readdir(&ctx, &cache, &log, &dir,
                                      collect_one, &collect, NULL));
    ASSERT_EQ(collect.count, 1);
    ASSERT_STR_EQ(collect.node.name, "LONGNAME10");
    ASSERT_EQ(collect.node.size, 5);

    odfs_cache_destroy(&cache);
}

TEST(udf_readdir_header_straddles_odd_sized_extents)
{
    udf_mock_media_t mock;
    odfs_media_t media;
    odfs_cache_t cache;
    odfs_log_state_t log;
    udf_context_t ctx;
    odfs_node_t dir;
    odfs_node_t out;
    collect_ctx_t collect;
    uint8_t fid[40];

    /* a 2000-byte extent in sector 1, then 80 bytes in sector 4 */
    static const uint32_t dir_ads[] = { 2000, 1, 80, 4 };

    memset(&mock, 0, sizeof(mock));
    memset(&ctx, 0, sizeof(ctx));
    memset(&dir, 0, sizeof(dir));
    memset(&collect, 0, sizeof(collect));

    udf_make_media(&media, &mock);
    ASSERT_OK(odfs_cache_init(&cache, &media, 4));
    odfs_log_init(&log);

    udf_make_fe_ads(mock.sectors[0], UDF_ICB_FILETYPE_DIR, 2080, dir_ads, 2);
    udf_make_deleted_fid(mock.sectors[1], 1980);

    /* "A" has its first 20 bytes at the end of the first extent */
    udf_make_fid(fid, "A", 5);
    memcpy(&mock.sectors[1][1980], fid, 20);
    memcpy(&mock.sectors[4][0], fid + 20, 20);
    udf_make_fid(fid, "B", 5);
    memcpy(&mock.sectors[4][20], fid, 40);

    udf_make_fe(mock.sectors[5], UDF_ICB_FILETYPE_FILE, 7, 6);

    dir.backend = ODFS_BACKEND_UDF;
    dir.kind = ODFS_NODE_DIR;
    dir.extent.lba = 0;

    ASSERT_OK(udf_backend_ops.readdir(&ctx, &cache, &log, &dir,
                                      collect_one, &collect, NULL));
    ASSERT_EQ(collect.count, 2);
    ASSERT_STR_EQ(collect.node.name, "B");
    ASSERT_OK(udf_backend_ops.lookup(&ctx, &cache, &log, &dir, "A", &out));
    ASSERT_EQ(out.size, 7);

    odfs_cache_destroy(&cache);
}

TEST(udf_readdir_embedded_directory)
{
    udf_mock_media_t mock;
    odfs_media_t media;
    odfs_cache_t cache;
    odfs_log_state_t log;
    udf_context_t ctx;
    odfs_node_t dir;
    collect_ctx_t collect;
    uint8_t fid[40];
    uint8_t *fe;

    memset(&mock, 0, sizeof(mock));
    memset(&ctx, 0, sizeof(ctx));
    memset(&dir, 0, sizeof(dir));
    memset(&collect, 0, sizeof(collect));

    udf_make_media(&media, &mock);
    ASSERT_OK(odfs_cache_init(&cache, &media, 4));
    odfs_log_init(&log);

    /* FIDs stored inside the directory's FE, after 4 bytes of EAs */
    fe = mock.sectors[0];
    udf_write_tag(fe, UDF_TAG_FE);
    fe[16 + 11] = UDF_ICB_FILETYPE_DIR;
    udf_write_le16(&fe[16 + 18], UDF_ICB_ALLOC_EMBEDDED);
    udf_write_le64(&fe[56], 80);
    udf_write_le32(&fe[168], 4);   /* ea_len */
    udf_write_le32(&fe[172], 80);  /* embedded data length */
    udf_make_fid(fid, "A", 3);
    memcpy(&fe[180], fid, sizeof(fid));
    udf_make_fid(fid, "B", 4);
    memcpy(&fe[220], fid, sizeof(fid));

    udf_make_fe(mock.sectors[3], UDF_ICB_FILETYPE_FILE, 5, 5);
    udf_make_fe(mock.sectors[4], UDF_ICB_FILETYPE_FILE, 7, 6);

    dir.backend = ODFS_BACKEND_UDF;
    dir.kind = ODFS_NODE_DIR;
    dir.extent.lba = 0;

    ASSERT_OK(udf_backend_ops.readdir(&ctx, &cache, &log, &dir,
                                      collect_one, &collect, NULL));
    ASSERT_EQ(collect.count, 2);
    ASSERT_STR_EQ(collect.node.name, "B");
    ASSERT_EQ(collect.node.size, 7);

    odfs_cache_destroy(&cache);
}

TEST(udf_readdir_reports_truncated_deleted_fid)
{
    udf_mock_media_t mock;
    odfs_media_t media;
    odfs_cache_t cache;
    odfs_log_state_t log;
    udf_context_t ctx;
    odfs_node_t dir;
    collect_ctx_t collect;
    uint8_t fid[40];

    memset(&mock, 0, sizeof(mock));
    memset(&ctx, 0, sizeof(ctx));
    memset(&dir, 0, sizeof(dir));
    memset(&collect, 0, sizeof(collect));

    udf_make_media(&media, &mock);
    ASSERT_OK(odfs_cache_init(&cache, &media, 4));
    odfs_log_init(&log);

    /* Information Length 2100, but only one sector is recorded: the
     * deleted FID at offset 40 runs past the recorded data */
    udf_make_fe(mock.sectors[0], UDF_ICB_FILETYPE_DIR, 2100, 1);
    udf_write_le32(&mock.sectors[0][176], 2048);
    udf_make_fid(fid, "A", 3);
    memcpy(&mock.sectors[1][0], fid, sizeof(fid));
    udf_make_deleted_fid(&mock.sectors[1][40], 2060);

    udf_make_fe(mock.sectors[3], UDF_ICB_FILETYPE_FILE, 5, 5);

    dir.backend = ODFS_BACKEND_UDF;
    dir.kind = ODFS_NODE_DIR;
    dir.extent.lba = 0;

    ASSERT_ERR(udf_backend_ops.readdir(&ctx, &cache, &log, &dir,
                                       collect_one, &collect, NULL),
               ODFS_ERR_EOF);

    odfs_cache_destroy(&cache);
}

/* directory FE in sector fe_lba with a ".." FID and one child dir FID */
static void udf_make_dir_level(udf_mock_media_t *mock, uint32_t fe_lba,
                               uint32_t parent_lba, const char *child,
                               uint32_t child_lba)
{
    uint8_t fid[40];
    uint32_t size = child ? 80 : 40;

    udf_make_fe(mock->sectors[fe_lba], UDF_ICB_FILETYPE_DIR, size,
                fe_lba + 1);
    udf_make_parent_fid(fid, parent_lba);
    memcpy(&mock->sectors[fe_lba + 1][0], fid, sizeof(fid));
    if (child) {
        udf_make_fid(fid, child, child_lba);
        fid[18] = UDF_FID_FLAG_DIRECTORY;
        udf_write_tag(fid, UDF_TAG_FID);
        memcpy(&mock->sectors[fe_lba + 1][40], fid, sizeof(fid));
    }
}

TEST(udf_resolve_parent_returns_parent_and_grandparent)
{
    udf_mock_media_t mock;
    odfs_media_t media;
    odfs_cache_t cache;
    odfs_log_state_t log;
    udf_context_t ctx;
    odfs_node_t a, b, c, parent, grandparent;

    memset(&mock, 0, sizeof(mock));
    memset(&ctx, 0, sizeof(ctx));

    udf_make_media(&media, &mock);
    ASSERT_OK(odfs_cache_init(&cache, &media, 4));
    odfs_log_init(&log);

    /* / (FE 0) -> a (FE 2) -> b (FE 4) -> c (FE 6) */
    udf_make_dir_level(&mock, 0, 0, "a", 2);
    udf_make_dir_level(&mock, 2, 0, "b", 4);
    udf_make_dir_level(&mock, 4, 2, "c", 6);
    udf_make_dir_level(&mock, 6, 4, NULL, 0);

    ctx.root.backend = ODFS_BACKEND_UDF;
    ctx.root.kind = ODFS_NODE_DIR;
    ctx.root.name[0] = '/';
    ctx.root.extent.lba = 0;

    ASSERT_OK(udf_backend_ops.lookup(&ctx, &cache, &log, &ctx.root, "a", &a));
    ASSERT_OK(udf_backend_ops.lookup(&ctx, &cache, &log, &a, "b", &b));
    ASSERT_OK(udf_backend_ops.lookup(&ctx, &cache, &log, &b, "c", &c));

    ASSERT_OK(udf_backend_ops.resolve_parent(&ctx, &cache, &log, &c,
                                             &parent, &grandparent));
    ASSERT_STR_EQ(parent.name, "b");
    ASSERT_EQ(parent.extent.lba, 4);
    ASSERT_STR_EQ(grandparent.name, "a");
    ASSERT_EQ(grandparent.extent.lba, 2);

    ASSERT_OK(udf_backend_ops.resolve_parent(&ctx, &cache, &log, &b,
                                             &parent, &grandparent));
    ASSERT_STR_EQ(parent.name, "a");
    ASSERT_STR_EQ(grandparent.name, "/");

    ASSERT_OK(udf_backend_ops.resolve_parent(&ctx, &cache, &log, &a,
                                             &parent, NULL));
    ASSERT_STR_EQ(parent.name, "/");

    ASSERT_ERR(udf_backend_ops.resolve_parent(&ctx, &cache, &log, &ctx.root,
                                              &parent, NULL),
               ODFS_ERR_NOT_FOUND);

    odfs_cache_destroy(&cache);
}

/* ---- mounting a whole volume ---- */

#define VOL_SECTOR_COUNT 300

typedef struct udf_vol_media {
    uint8_t  sectors[VOL_SECTOR_COUNT][MOCK_SECTOR_SIZE];
    uint32_t reads;
} udf_vol_media_t;

static odfs_err_t udf_vol_read_sectors(void *ctx, uint32_t lba,
                                       uint32_t count, void *buf)
{
    udf_vol_media_t *m = ctx;

    m->reads += count;
    if (lba + count > VOL_SECTOR_COUNT)
        return ODFS_ERR_RANGE;
    memcpy(buf, &m->sectors[lba][0], (size_t)count * MOCK_SECTOR_SIZE);
    return ODFS_OK;
}

static uint32_t udf_vol_sector_count(void *ctx)
{
    (void)ctx;
    return VOL_SECTOR_COUNT;
}

static const odfs_media_ops_t udf_vol_ops = {
    .read_sectors = udf_vol_read_sectors,
    .sector_size  = udf_mock_sector_size,
    .sector_count = udf_vol_sector_count,
};

#define VOL_PART_START 100

/*
 * A minimal volume: AVDP at 256 pointing at a VDS in sectors 32..36
 * (PVD "VOL", PD number 7 at sector 100, LVD, TD). The LVD has one
 * Type 1 partition map; the FSD is at partition block 0 and the root
 * directory File Entry, with no entries, at partition block 1. Returns
 * the LVD so that callers can change it.
 */
static uint8_t *udf_vol_build(udf_vol_media_t *m)
{
    uint8_t *lvd = m->sectors[35];
    uint8_t *pd = m->sectors[34];
    uint8_t *pvd = m->sectors[32];

    memset(m, 0, sizeof(*m));

    udf_write_tag(m->sectors[UDF_AVDP_LBA], UDF_TAG_AVDP);
    udf_write_le32(&m->sectors[UDF_AVDP_LBA][16], 16 * MOCK_SECTOR_SIZE);
    udf_write_le32(&m->sectors[UDF_AVDP_LBA][20], 32);

    udf_write_tag(pvd, UDF_TAG_PVD);
    pvd[24] = 8;                  /* CS0 8-bit */
    memcpy(&pvd[25], "VOL", 3);
    pvd[24 + 31] = 4;             /* d-string used length */

    udf_write_tag(pd, UDF_TAG_PD);
    udf_write_le16(&pd[22], 7);   /* partition number */
    udf_write_le32(&pd[188], VOL_PART_START);
    udf_write_le32(&pd[192], 150);

    udf_write_tag(lvd, UDF_TAG_LVD);
    udf_write_le32(&lvd[212], MOCK_SECTOR_SIZE);
    udf_write_le32(&lvd[248], MOCK_SECTOR_SIZE); /* FSD long_ad */
    udf_write_le32(&lvd[252], 0);
    udf_write_le16(&lvd[256], 0);
    udf_write_le32(&lvd[264], 6); /* map table length */
    udf_write_le32(&lvd[268], 1); /* number of partition maps */
    lvd[440] = 1;                 /* Type 1 map */
    lvd[441] = 6;
    udf_write_le16(&lvd[442], 1); /* volume sequence number */
    udf_write_le16(&lvd[444], 7); /* partition number */

    udf_write_tag(m->sectors[36], UDF_TAG_TD);

    udf_write_tag(m->sectors[VOL_PART_START], UDF_TAG_FSD);
    udf_write_le32(&m->sectors[VOL_PART_START][400], MOCK_SECTOR_SIZE);
    udf_write_le32(&m->sectors[VOL_PART_START][404], 1);

    udf_make_fe(m->sectors[VOL_PART_START + 1], UDF_ICB_FILETYPE_DIR, 0, 2);
    return lvd;
}

TEST(udf_mount_rejects_non_2048_logical_blocks)
{
    static udf_vol_media_t m;
    odfs_media_t media = { .ops = &udf_vol_ops, .ctx = &m };
    odfs_cache_t cache;
    odfs_log_state_t log;
    odfs_node_t root;
    void *ctx = NULL;
    uint8_t *lvd;
    char name[16];

    odfs_log_init(&log);
    ASSERT_OK(odfs_cache_init(&cache, &media, 16));

    lvd = udf_vol_build(&m);
    ASSERT_OK(udf_backend_ops.mount(&cache, &log, 0, &root, &ctx));
    ASSERT_OK(udf_backend_ops.get_volume_name(ctx, name, sizeof(name)));
    ASSERT_STR_EQ(name, "VOL");
    ASSERT_EQ(udf_backend_ops.get_volume_size(ctx), 150);
    udf_backend_ops.unmount(ctx);

    udf_write_le32(&lvd[212], 4096);
    odfs_cache_flush(&cache);
    ctx = NULL;
    ASSERT_ERR(udf_backend_ops.mount(&cache, &log, 0, &root, &ctx),
               ODFS_ERR_UNSUPPORTED);
    ASSERT(ctx == NULL);

    odfs_cache_destroy(&cache);
}

TEST(udf_mount_bounds_the_vds_scan)
{
    static udf_vol_media_t m;
    odfs_media_t media = { .ops = &udf_vol_ops, .ctx = &m };
    odfs_cache_t cache;
    odfs_log_state_t log;
    odfs_node_t root;
    void *ctx = NULL;

    odfs_log_init(&log);
    ASSERT_OK(odfs_cache_init(&cache, &media, 16));

    /* a VDS extent of almost 4 GiB without a Terminating Descriptor */
    udf_vol_build(&m);
    memset(m.sectors[36], 0, MOCK_SECTOR_SIZE);
    udf_write_le32(&m.sectors[UDF_AVDP_LBA][16], 0xFFFFF800u);
    m.reads = 0;

    ASSERT_OK(udf_backend_ops.mount(&cache, &log, 0, &root, &ctx));
    ASSERT(m.reads < 1024);
    udf_backend_ops.unmount(ctx);

    odfs_cache_destroy(&cache);
}

TEST_MAIN()
