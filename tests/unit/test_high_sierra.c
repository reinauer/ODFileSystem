/*
 * test_high_sierra.c — tests for High Sierra support in the ISO 9660 backend
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Builds a minimal synthetic High Sierra image in memory and mounts it
 * through the full core mount path. Field offsets mirror the OS/2 Warp 4
 * install CD, the reference High Sierra medium used during development.
 */

#include "iso9660/iso9660.h"
#include "odfs/api.h"
#include "odfs/error.h"
#include "test_harness.h"

#include <string.h>

#if ODFS_FEATURE_HIGH_SIERRA

#define HS_TEST_SECTORS      32
#define HS_ROOT_LBA          20
#define HS_SUB_LBA           21
#define HS_README_LBA        22
#define HS_INNER_LBA         23

static const char hs_readme_data[] = "High Sierra!\n";   /* 13 bytes + NUL */
static const char hs_inner_data[]  = "abcde";

typedef struct hs_media {
    uint8_t sectors[HS_TEST_SECTORS][ISO_SECTOR_SIZE];
} hs_media_t;

static odfs_err_t hs_read_sectors(void *ctx, uint32_t lba,
                                  uint32_t count, void *buf)
{
    hs_media_t *media = ctx;

    if (lba + count > HS_TEST_SECTORS)
        return ODFS_ERR_EOF;

    memcpy(buf, &media->sectors[lba][0], (size_t)count * ISO_SECTOR_SIZE);
    return ODFS_OK;
}

static uint32_t hs_sector_size(void *ctx)
{
    (void)ctx;
    return ISO_SECTOR_SIZE;
}

static uint32_t hs_sector_count(void *ctx)
{
    (void)ctx;
    return HS_TEST_SECTORS;
}

static const odfs_media_ops_t hs_media_ops = {
    .read_sectors = hs_read_sectors,
    .sector_size = hs_sector_size,
    .sector_count = hs_sector_count,
};

/* write a 32-bit both-byte-order field (7.3.3) */
static void hs_wr_bb32(uint8_t *p, uint32_t v)
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

/* write a 16-bit both-byte-order field (7.2.3) */
static void hs_wr_bb16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/*
 * Write a High Sierra directory record: 6-byte date at offset 18,
 * flags at offset 24. Returns the record length.
 */
static size_t hs_dir_record(uint8_t *p, uint32_t lba, uint32_t size,
                            uint8_t flags, const char *name, size_t name_len)
{
    size_t rec_len = 33 + name_len + ((name_len & 1) == 0 ? 1 : 0);

    memset(p, 0, rec_len);
    p[0] = (uint8_t)rec_len;
    hs_wr_bb32(&p[ISO_DR_EXTENT_LBA], lba);
    hs_wr_bb32(&p[ISO_DR_DATA_LENGTH], size);
    p[HS_DR_DATE + 0] = 86;   /* 1986 */
    p[HS_DR_DATE + 1] = 3;
    p[HS_DR_DATE + 2] = 17;
    p[HS_DR_DATE + 3] = 12;
    p[HS_DR_DATE + 4] = 30;
    p[HS_DR_DATE + 5] = 45;
    p[HS_DR_FLAGS] = flags;
    hs_wr_bb16(&p[28], 1);    /* volume sequence number */
    p[ISO_DR_NAME_LEN] = (uint8_t)name_len;
    memcpy(&p[ISO_DR_NAME], name, name_len);
    return rec_len;
}

static void hs_build_image(hs_media_t *media)
{
    uint8_t *pvd = media->sectors[ISO_VD_START_LBA];
    uint8_t *term = media->sectors[ISO_VD_START_LBA + 1];
    uint8_t *root = media->sectors[HS_ROOT_LBA];
    uint8_t *sub = media->sectors[HS_SUB_LBA];
    size_t off;

    memset(media, 0, sizeof(*media));

    /* --- SFS primary volume descriptor at sector 16 --- */
    hs_wr_bb32(&pvd[0], ISO_VD_START_LBA);          /* descriptor LBN */
    pvd[HS_VD_TYPE] = ISO_VD_TYPE_PRIMARY;
    memcpy(&pvd[HS_VD_ID], HS_STANDARD_ID, HS_STANDARD_ID_LEN);
    pvd[HS_VD_VERSION] = 1;
    memset(&pvd[HS_PVD_SYSTEM_ID], ' ', 32);
    memset(&pvd[HS_PVD_VOLUME_ID], ' ', 32);
    memcpy(&pvd[HS_PVD_VOLUME_ID], "HSTEST", 6);
    hs_wr_bb32(&pvd[HS_PVD_VOLUME_SPACE_SIZE], HS_TEST_SECTORS);
    hs_wr_bb16(&pvd[HS_PVD_LOGICAL_BLK_SIZE], ISO_SECTOR_SIZE);
    hs_wr_bb32(&pvd[HS_PVD_PATH_TABLE_SIZE], 0);
    hs_dir_record(&pvd[HS_PVD_ROOT_DIR_RECORD], HS_ROOT_LBA, ISO_SECTOR_SIZE,
                  ISO_DR_FLAG_DIRECTORY, "\x00", 1);

    /* --- volume descriptor set terminator at sector 17 --- */
    hs_wr_bb32(&term[0], ISO_VD_START_LBA + 1);
    term[HS_VD_TYPE] = ISO_VD_TYPE_TERM;
    memcpy(&term[HS_VD_ID], HS_STANDARD_ID, HS_STANDARD_ID_LEN);
    term[HS_VD_VERSION] = 1;

    /* --- root directory extent --- */
    off = 0;
    off += hs_dir_record(&root[off], HS_ROOT_LBA, ISO_SECTOR_SIZE,
                         ISO_DR_FLAG_DIRECTORY, "\x00", 1);          /* .  */
    off += hs_dir_record(&root[off], HS_ROOT_LBA, ISO_SECTOR_SIZE,
                         ISO_DR_FLAG_DIRECTORY, "\x01", 1);          /* .. */
    off += hs_dir_record(&root[off], HS_README_LBA,
                         sizeof(hs_readme_data) - 1, 0,
                         "README.TXT;1", 12);
    off += hs_dir_record(&root[off], HS_SUB_LBA, ISO_SECTOR_SIZE,
                         ISO_DR_FLAG_DIRECTORY, "SUB", 3);

    /* --- SUB directory extent --- */
    off = 0;
    off += hs_dir_record(&sub[off], HS_SUB_LBA, ISO_SECTOR_SIZE,
                         ISO_DR_FLAG_DIRECTORY, "\x00", 1);          /* .  */
    off += hs_dir_record(&sub[off], HS_ROOT_LBA, ISO_SECTOR_SIZE,
                         ISO_DR_FLAG_DIRECTORY, "\x01", 1);          /* .. */
    off += hs_dir_record(&sub[off], HS_INNER_LBA,
                         sizeof(hs_inner_data) - 1, 0,
                         "INNER.DAT;1", 11);

    /* --- file data --- */
    memcpy(media->sectors[HS_README_LBA], hs_readme_data,
           sizeof(hs_readme_data) - 1);
    memcpy(media->sectors[HS_INNER_LBA], hs_inner_data,
           sizeof(hs_inner_data) - 1);
}

static odfs_err_t hs_mount_image(hs_media_t *media, odfs_mount_t *mnt)
{
    odfs_media_t m;

    hs_build_image(media);
    m.ops = &hs_media_ops;
    m.ctx = media;
    return odfs_mount(&m, NULL, NULL, mnt);
}

typedef struct hs_collect {
    int count;
    odfs_node_t entries[8];
} hs_collect_t;

static odfs_err_t hs_collect_cb(const odfs_node_t *entry, void *ctx)
{
    hs_collect_t *c = ctx;

    if (c->count < 8)
        c->entries[c->count] = *entry;
    c->count++;
    return ODFS_OK;
}

TEST(high_sierra_mounts_as_iso9660)
{
    static hs_media_t media;
    odfs_mount_t mnt;

    ASSERT_OK(hs_mount_image(&media, &mnt));
    ASSERT_EQ(mnt.active_backend, ODFS_BACKEND_ISO9660);
    ASSERT_STR_EQ(mnt.volume_name, "HSTEST");
    ASSERT_EQ(mnt.total_blocks, HS_TEST_SECTORS);
    odfs_unmount(&mnt);
}

TEST(high_sierra_readdir_root)
{
    static hs_media_t media;
    odfs_mount_t mnt;
    hs_collect_t collect;

    ASSERT_OK(hs_mount_image(&media, &mnt));

    memset(&collect, 0, sizeof(collect));
    ASSERT_OK(odfs_readdir(&mnt, &mnt.root, hs_collect_cb, &collect, NULL));
    ASSERT_EQ(collect.count, 2);
    ASSERT_STR_EQ(collect.entries[0].name, "README.TXT");
    ASSERT_EQ(collect.entries[0].kind, ODFS_NODE_FILE);
    ASSERT_EQ(collect.entries[0].size, sizeof(hs_readme_data) - 1);
    ASSERT_STR_EQ(collect.entries[1].name, "SUB");
    ASSERT_EQ(collect.entries[1].kind, ODFS_NODE_DIR);

    /* 6-byte High Sierra date: no timezone byte to misread */
    ASSERT_EQ(collect.entries[0].mtime.year, 1986);
    ASSERT_EQ(collect.entries[0].mtime.month, 3);
    ASSERT_EQ(collect.entries[0].mtime.day, 17);
    ASSERT_EQ(collect.entries[0].mtime.second, 45);
    ASSERT_EQ(collect.entries[0].mtime.tz_offset, 0);

    odfs_unmount(&mnt);
}

TEST(high_sierra_read_file)
{
    static hs_media_t media;
    odfs_mount_t mnt;
    odfs_node_t file;
    char buf[32];
    size_t len;

    ASSERT_OK(hs_mount_image(&media, &mnt));

    ASSERT_OK(odfs_resolve_path(&mnt, "README.TXT", &file));
    ASSERT_EQ(file.kind, ODFS_NODE_FILE);

    memset(buf, 0, sizeof(buf));
    len = sizeof(buf);
    ASSERT_OK(odfs_read(&mnt, &file, 0, buf, &len));
    ASSERT_EQ(len, sizeof(hs_readme_data) - 1);
    ASSERT(memcmp(buf, hs_readme_data, len) == 0);

    odfs_unmount(&mnt);
}

TEST(high_sierra_subdir_traversal)
{
    static hs_media_t media;
    odfs_mount_t mnt;
    odfs_node_t file;
    char buf[16];
    size_t len;

    ASSERT_OK(hs_mount_image(&media, &mnt));

    ASSERT_OK(odfs_resolve_path(&mnt, "SUB/INNER.DAT", &file));
    ASSERT_EQ(file.kind, ODFS_NODE_FILE);
    ASSERT_EQ(file.size, sizeof(hs_inner_data) - 1);

    memset(buf, 0, sizeof(buf));
    len = sizeof(buf);
    ASSERT_OK(odfs_read(&mnt, &file, 0, buf, &len));
    ASSERT_EQ(len, sizeof(hs_inner_data) - 1);
    ASSERT(memcmp(buf, hs_inner_data, len) == 0);

    odfs_unmount(&mnt);
}

TEST(high_sierra_offsets)
{
    ASSERT_EQ(memcmp(HS_STANDARD_ID, "CDROM", 5), 0);
    ASSERT_EQ(HS_VD_TYPE, 8);
    ASSERT_EQ(HS_VD_ID, 9);
    ASSERT_EQ(HS_PVD_VOLUME_ID, 48);
    ASSERT_EQ(HS_PVD_VOLUME_SPACE_SIZE, 88);
    ASSERT_EQ(HS_PVD_LOGICAL_BLK_SIZE, 136);
    ASSERT_EQ(HS_PVD_ROOT_DIR_RECORD, 180);
    ASSERT_EQ(HS_DR_DATE, 18);
    ASSERT_EQ(HS_DR_FLAGS, 24);
}

#else /* !ODFS_FEATURE_HIGH_SIERRA */

TEST(high_sierra_disabled)
{
    /* feature compiled out — nothing to verify */
    ASSERT(1);
}

#endif /* ODFS_FEATURE_HIGH_SIERRA */

TEST_MAIN()
