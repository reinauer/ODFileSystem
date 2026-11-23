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

TEST_MAIN()
