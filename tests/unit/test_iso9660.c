/*
 * test_iso9660.c — tests for ISO 9660 backend
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "odfs/api.h"
#include "odfs/error.h"
#include "iso9660/iso9660.h"
#include "odfs/cache.h"
#include "test_harness.h"

#include <string.h>

/* ---- endian / field helpers ---- */

TEST(iso_read_le16_basic)
{
    uint8_t buf[] = { 0x34, 0x12, 0x12, 0x34 };
    ASSERT_EQ(iso_read_le16(buf), 0x1234);
}

TEST(iso_read_le32_basic)
{
    uint8_t buf[] = { 0x78, 0x56, 0x34, 0x12, 0x12, 0x34, 0x56, 0x78 };
    ASSERT_EQ(iso_read_le32(buf), 0x12345678);
}

TEST(iso_read_le32_zero)
{
    uint8_t buf[8] = { 0 };
    ASSERT_EQ(iso_read_le32(buf), 0);
}

TEST(iso_read_le32_max)
{
    uint8_t buf[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    ASSERT_EQ(iso_read_le32(buf), 0xFFFFFFFF);
}

/* ---- PVD signature recognition ---- */

TEST(iso_pvd_signature)
{
    /* verify the standard identifier constant */
    ASSERT_EQ(ISO_STANDARD_ID_LEN, 5);
    ASSERT_EQ(memcmp(ISO_STANDARD_ID, "CD001", 5), 0);
}

TEST(iso_vd_type_constants)
{
    ASSERT_EQ(ISO_VD_TYPE_PRIMARY, 1);
    ASSERT_EQ(ISO_VD_TYPE_SUPPL, 2);
    ASSERT_EQ(ISO_VD_TYPE_TERM, 255);
}

/* ---- directory record flags ---- */

TEST(iso_dr_flag_bits)
{
    ASSERT_EQ(ISO_DR_FLAG_HIDDEN, 0x01);
    ASSERT_EQ(ISO_DR_FLAG_DIRECTORY, 0x02);
    ASSERT_EQ(ISO_DR_FLAG_ASSOCIATED, 0x04);
    ASSERT_EQ(ISO_DR_FLAG_MULTI_EXTENT, 0x80);
}

/* ---- backend ops table ---- */

TEST(iso_backend_ops_valid)
{
    ASSERT(iso9660_backend_ops.name != NULL);
    ASSERT_STR_EQ(iso9660_backend_ops.name, "iso9660");
    ASSERT_EQ(iso9660_backend_ops.backend_type, ODFS_BACKEND_ISO9660);
    ASSERT(iso9660_backend_ops.probe != NULL);
    ASSERT(iso9660_backend_ops.mount != NULL);
    ASSERT(iso9660_backend_ops.unmount != NULL);
    ASSERT(iso9660_backend_ops.readdir != NULL);
    ASSERT(iso9660_backend_ops.read != NULL);
    ASSERT(iso9660_backend_ops.lookup != NULL);
    ASSERT(iso9660_backend_ops.get_volume_name != NULL);
}

/* ---- PVD field offsets ---- */

TEST(iso_pvd_offsets)
{
    /* verify critical offsets match ECMA-119 */
    ASSERT_EQ(ISO_PVD_TYPE, 0);
    ASSERT_EQ(ISO_PVD_ID, 1);
    ASSERT_EQ(ISO_PVD_VOLUME_ID, 40);
    ASSERT_EQ(ISO_PVD_VOLUME_SPACE_SIZE, 80);
    ASSERT_EQ(ISO_PVD_LOGICAL_BLK_SIZE, 128);
    ASSERT_EQ(ISO_PVD_ROOT_DIR_RECORD, 156);
}

/* ---- directory record offsets ---- */

TEST(iso_dr_offsets)
{
    ASSERT_EQ(ISO_DR_LENGTH, 0);
    ASSERT_EQ(ISO_DR_EXTENT_LBA, 2);
    ASSERT_EQ(ISO_DR_DATA_LENGTH, 10);
    ASSERT_EQ(ISO_DR_DATE, 18);
    ASSERT_EQ(ISO_DR_FLAGS, 25);
    ASSERT_EQ(ISO_DR_NAME_LEN, 32);
    ASSERT_EQ(ISO_DR_NAME, 33);
}

/* ---- directory scan bounds ---- */

/* every sector reads as zeros; fail once a scan reads far past its end */
static uint32_t zero_media_reads;

static odfs_err_t zero_media_read_sectors(void *ctx, uint32_t lba,
                                          uint32_t count, void *buf)
{
    (void)ctx;
    (void)lba;
    zero_media_reads += count;
    if (zero_media_reads > (1u << 21) + 1024u)
        return ODFS_ERR_IO;
    memset(buf, 0, (size_t)count * ISO_SECTOR_SIZE);
    return ODFS_OK;
}

static uint32_t zero_media_sector_size(void *ctx)
{
    (void)ctx;
    return ISO_SECTOR_SIZE;
}

static uint32_t zero_media_sector_count(void *ctx)
{
    (void)ctx;
    return 0xFFFFFFFFu;
}

static const odfs_media_ops_t zero_media_ops = {
    .read_sectors = zero_media_read_sectors,
    .sector_size  = zero_media_sector_size,
    .sector_count = zero_media_sector_count,
};

static odfs_err_t count_entries_cb(const odfs_node_t *entry, void *arg)
{
    (void)entry;
    (*(int *)arg)++;
    return ODFS_OK;
}

TEST(iso_readdir_dir_size_near_uint32_max_terminates)
{
    odfs_media_t media = { .ops = &zero_media_ops, .ctx = NULL };
    odfs_cache_t cache;
    odfs_log_state_t log;
    iso_context_t ctx;
    odfs_node_t dir;
    int entries = 0;

    memset(&ctx, 0, sizeof(ctx));
    memset(&dir, 0, sizeof(dir));
    dir.kind = ODFS_NODE_DIR;
    dir.extent.lba = 0;
    dir.extent.length = 0xFFFFFFFFu;
    dir.size = dir.extent.length;

    odfs_log_init(&log);
    ASSERT_OK(odfs_cache_init(&cache, &media, 8));
    zero_media_reads = 0;

    /* skipping the empty last sector must end the scan, not wrap to 0 */
    ASSERT_OK(odfs_iso_readdir(&ctx, &cache, &log, &dir,
                               count_entries_cb, &entries, NULL));
    ASSERT_EQ(entries, 0);

    odfs_cache_destroy(&cache);
}

/* ---- synthetic ISO media ---- */

#define ISO_TEST_SECTORS 32

typedef struct iso_test_media {
    uint8_t sectors[ISO_TEST_SECTORS][ISO_SECTOR_SIZE];
} iso_test_media_t;

static odfs_err_t iso_test_read_sectors(void *ctx, uint32_t lba,
                                        uint32_t count, void *buf)
{
    iso_test_media_t *m = ctx;
    if (lba >= ISO_TEST_SECTORS || count > ISO_TEST_SECTORS - lba)
        return ODFS_ERR_EOF;
    memcpy(buf, &m->sectors[lba][0], (size_t)count * ISO_SECTOR_SIZE);
    return ODFS_OK;
}

static uint32_t iso_test_sector_size(void *ctx)
{
    (void)ctx;
    return ISO_SECTOR_SIZE;
}

static uint32_t iso_test_sector_count(void *ctx)
{
    (void)ctx;
    return ISO_TEST_SECTORS;
}

static const odfs_media_ops_t iso_test_media_ops = {
    .read_sectors = iso_test_read_sectors,
    .sector_size  = iso_test_sector_size,
    .sector_count = iso_test_sector_count,
};

static void iso_wr_bb16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void iso_wr_bb32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
    p[4] = (uint8_t)(v >> 24);
    p[5] = (uint8_t)(v >> 16);
    p[6] = (uint8_t)(v >> 8);
    p[7] = (uint8_t)v;
}

static size_t iso_wr_rec(uint8_t *p, uint32_t lba, uint8_t ear_len,
                         uint32_t size, uint8_t flags,
                         const char *name, uint8_t name_len)
{
    uint8_t len = (uint8_t)(33 + name_len + ((name_len & 1) == 0 ? 1 : 0));
    memset(p, 0, len);
    p[ISO_DR_LENGTH] = len;
    p[ISO_DR_EXT_ATTR_LENGTH] = ear_len;
    iso_wr_bb32(&p[ISO_DR_EXTENT_LBA], lba);
    iso_wr_bb32(&p[ISO_DR_DATA_LENGTH], size);
    p[ISO_DR_DATE] = 126;
    p[ISO_DR_DATE + 1] = 1;
    p[ISO_DR_DATE + 2] = 1;
    p[ISO_DR_FLAGS] = flags;
    iso_wr_bb16(&p[ISO_DR_VOLUME_SEQ_NUM], 1);
    p[ISO_DR_NAME_LEN] = name_len;
    memcpy(&p[ISO_DR_NAME], name, name_len);
    return len;
}

static void iso_init_pvd(iso_test_media_t *m, uint16_t blk_size,
                         uint32_t root_lbn, uint8_t root_ear)
{
    uint8_t *pvd = m->sectors[ISO_VD_START_LBA];
    uint8_t *term = m->sectors[ISO_VD_START_LBA + 1];

    memset(m, 0, sizeof(*m));
    pvd[ISO_PVD_TYPE] = ISO_VD_TYPE_PRIMARY;
    memcpy(&pvd[ISO_PVD_ID], ISO_STANDARD_ID, ISO_STANDARD_ID_LEN);
    pvd[ISO_PVD_VERSION] = 1;
    memset(&pvd[ISO_PVD_SYSTEM_ID], ' ', 32);
    memset(&pvd[ISO_PVD_VOLUME_ID], ' ', 32);
    memcpy(&pvd[ISO_PVD_VOLUME_ID], "ISOTEST", 7);
    iso_wr_bb32(&pvd[ISO_PVD_VOLUME_SPACE_SIZE],
                ISO_TEST_SECTORS * (ISO_SECTOR_SIZE / blk_size));
    iso_wr_bb16(&pvd[ISO_PVD_LOGICAL_BLK_SIZE], blk_size);
    iso_wr_rec(&pvd[ISO_PVD_ROOT_DIR_RECORD], root_lbn, root_ear,
               ISO_SECTOR_SIZE, ISO_DR_FLAG_DIRECTORY, "\x00", 1);

    term[ISO_PVD_TYPE] = ISO_VD_TYPE_TERM;
    memcpy(&term[ISO_PVD_ID], ISO_STANDARD_ID, ISO_STANDARD_ID_LEN);
    term[ISO_PVD_VERSION] = 1;
}

TEST(iso_extent_skips_extended_attribute_record)
{
    static iso_test_media_t m;
    odfs_media_t media = { .ops = &iso_test_media_ops, .ctx = &m };
    odfs_cache_t cache;
    odfs_log_state_t log;
    odfs_node_t root, file;
    void *ctx = NULL;
    char buf[16];
    size_t len, off = 0;

    /* root extent at LBA 18 with a 2-block EAR: directory data at LBA 20 */
    iso_init_pvd(&m, ISO_SECTOR_SIZE, 18, 2);
    off += iso_wr_rec(&m.sectors[20][off], 18, 2, ISO_SECTOR_SIZE,
                      ISO_DR_FLAG_DIRECTORY, "\x00", 1);
    off += iso_wr_rec(&m.sectors[20][off], 18, 2, ISO_SECTOR_SIZE,
                      ISO_DR_FLAG_DIRECTORY, "\x01", 1);
    /* file extent at LBA 22 with a 3-block EAR: file data at LBA 25 */
    off += iso_wr_rec(&m.sectors[20][off], 22, 3, 5, 0, "EAR.TXT;1", 9);
    memcpy(m.sectors[22], "WRONG", 5);
    memcpy(m.sectors[25], "EAROK", 5);

    odfs_log_init(&log);
    ASSERT_OK(odfs_cache_init(&cache, &media, 8));
    ASSERT_OK(iso9660_backend_ops.mount(&cache, &log, 0, &root, &ctx));
    ASSERT_EQ(root.extent.lba, 20);

    ASSERT_OK(iso9660_backend_ops.lookup(ctx, &cache, &log, &root,
                                         "EAR.TXT", &file));
    ASSERT_EQ(file.extent.lba, 25);

    len = sizeof(buf);
    memset(buf, 0, sizeof(buf));
    ASSERT_OK(iso9660_backend_ops.read(ctx, &cache, &log, &file, 0, buf, &len));
    ASSERT_EQ(len, 5);
    ASSERT(memcmp(buf, "EAROK", 5) == 0);

    iso9660_backend_ops.unmount(ctx);
    odfs_cache_destroy(&cache);
}

TEST(iso_node_ids_are_stable_across_scans)
{
    static iso_test_media_t m;
    odfs_media_t media = { .ops = &iso_test_media_ops, .ctx = &m };
    odfs_cache_t cache;
    odfs_log_state_t log;
    odfs_node_t root, a1, a2, b, parent;
    void *ctx = NULL;
    size_t off;

    /* root (20) -> A (21) -> B (22) */
    iso_init_pvd(&m, ISO_SECTOR_SIZE, 20, 0);
    off = 0;
    off += iso_wr_rec(&m.sectors[20][off], 20, 0, ISO_SECTOR_SIZE,
                      ISO_DR_FLAG_DIRECTORY, "\x00", 1);
    off += iso_wr_rec(&m.sectors[20][off], 20, 0, ISO_SECTOR_SIZE,
                      ISO_DR_FLAG_DIRECTORY, "\x01", 1);
    off += iso_wr_rec(&m.sectors[20][off], 21, 0, ISO_SECTOR_SIZE,
                      ISO_DR_FLAG_DIRECTORY, "A", 1);
    off = 0;
    off += iso_wr_rec(&m.sectors[21][off], 21, 0, ISO_SECTOR_SIZE,
                      ISO_DR_FLAG_DIRECTORY, "\x00", 1);
    off += iso_wr_rec(&m.sectors[21][off], 20, 0, ISO_SECTOR_SIZE,
                      ISO_DR_FLAG_DIRECTORY, "\x01", 1);
    off += iso_wr_rec(&m.sectors[21][off], 22, 0, ISO_SECTOR_SIZE,
                      ISO_DR_FLAG_DIRECTORY, "B", 1);
    off = 0;
    off += iso_wr_rec(&m.sectors[22][off], 22, 0, ISO_SECTOR_SIZE,
                      ISO_DR_FLAG_DIRECTORY, "\x00", 1);
    off += iso_wr_rec(&m.sectors[22][off], 21, 0, ISO_SECTOR_SIZE,
                      ISO_DR_FLAG_DIRECTORY, "\x01", 1);

    odfs_log_init(&log);
    ASSERT_OK(odfs_cache_init(&cache, &media, 8));
    ASSERT_OK(iso9660_backend_ops.mount(&cache, &log, 0, &root, &ctx));

    ASSERT_OK(iso9660_backend_ops.lookup(ctx, &cache, &log, &root, "A", &a1));
    ASSERT_OK(iso9660_backend_ops.lookup(ctx, &cache, &log, &root, "A", &a2));
    ASSERT(a1.id != 0);
    ASSERT_EQ(a1.id, a2.id);

    /* the parent found by walking up is the same node */
    ASSERT_OK(iso9660_backend_ops.lookup(ctx, &cache, &log, &a1, "B", &b));
    ASSERT_OK(iso9660_backend_ops.resolve_parent(ctx, &cache, &log, &b,
                                                 &parent, NULL));
    ASSERT_EQ(parent.extent.lba, 21);
    ASSERT_EQ(parent.id, a1.id);

    iso9660_backend_ops.unmount(ctx);
    odfs_cache_destroy(&cache);
}

TEST(iso_small_logical_blocks_map_to_sectors)
{
    static iso_test_media_t m;
    odfs_media_t media = { .ops = &iso_test_media_ops, .ctx = &m };
    odfs_cache_t cache;
    odfs_log_state_t log;
    odfs_node_t root, file;
    void *ctx = NULL;
    char buf[16];
    size_t len, off = 0;

    odfs_log_init(&log);
    ASSERT_OK(odfs_cache_init(&cache, &media, 8));

    /* 512-byte logical blocks: LBN 80 is sector 20 */
    iso_init_pvd(&m, 512, 20 * 4, 0);
    off += iso_wr_rec(&m.sectors[20][off], 20 * 4, 0, ISO_SECTOR_SIZE,
                      ISO_DR_FLAG_DIRECTORY, "\x00", 1);
    off += iso_wr_rec(&m.sectors[20][off], 20 * 4, 0, ISO_SECTOR_SIZE,
                      ISO_DR_FLAG_DIRECTORY, "\x01", 1);
    off += iso_wr_rec(&m.sectors[20][off], 25 * 4, 0, 5, 0, "A.TXT;1", 7);
    /* starts 512 bytes into sector 25: cannot be represented, skipped */
    off += iso_wr_rec(&m.sectors[20][off], 25 * 4 + 1, 0, 5, 0,
                      "B.TXT;1", 7);
    memcpy(m.sectors[25], "BLK_A", 5);

    ASSERT_OK(iso9660_backend_ops.mount(&cache, &log, 0, &root, &ctx));
    ASSERT_EQ(root.extent.lba, 20);
    ASSERT_EQ(iso9660_backend_ops.get_volume_size(ctx), ISO_TEST_SECTORS);

    ASSERT_OK(iso9660_backend_ops.lookup(ctx, &cache, &log, &root,
                                         "A.TXT", &file));
    ASSERT_EQ(file.extent.lba, 25);
    len = sizeof(buf);
    memset(buf, 0, sizeof(buf));
    ASSERT_OK(iso9660_backend_ops.read(ctx, &cache, &log, &file, 0, buf, &len));
    ASSERT_EQ(len, 5);
    ASSERT(memcmp(buf, "BLK_A", 5) == 0);

    ASSERT_ERR(iso9660_backend_ops.lookup(ctx, &cache, &log, &root,
                                          "B.TXT", &file),
               ODFS_ERR_NOT_FOUND);
    iso9660_backend_ops.unmount(ctx);

    /* an unknown block size is reported but mounted as 2048 */
    iso_init_pvd(&m, 4096, 20, 0);
    iso_wr_rec(&m.sectors[20][0], 20, 0, ISO_SECTOR_SIZE,
               ISO_DR_FLAG_DIRECTORY, "\x00", 1);
    odfs_cache_flush(&cache);
    ASSERT_OK(iso9660_backend_ops.mount(&cache, &log, 0, &root, &ctx));
    ASSERT_EQ(root.extent.lba, 20);
    iso9660_backend_ops.unmount(ctx);

    odfs_cache_destroy(&cache);
}

TEST_MAIN()
