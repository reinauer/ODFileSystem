/*
 * test_joliet.c — tests for Joliet backend edge cases
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "joliet/joliet.h"
#include "odfs/cache.h"
#include "odfs/error.h"
#include "test_harness.h"

#include <string.h>

#define TEST_SECTOR_SIZE 2048
#define TEST_SECTORS     256

typedef struct mem_media {
    uint8_t sectors[TEST_SECTORS][TEST_SECTOR_SIZE];
} mem_media_t;

static odfs_err_t mem_read_sectors(void *ctx, uint32_t lba,
                                   uint32_t count, void *buf)
{
    mem_media_t *media = ctx;

    if (lba + count > TEST_SECTORS)
        return ODFS_ERR_EOF;

    memcpy(buf, &media->sectors[lba][0], (size_t)count * TEST_SECTOR_SIZE);
    return ODFS_OK;
}

static uint32_t mem_sector_size(void *ctx)
{
    (void)ctx;
    return TEST_SECTOR_SIZE;
}

static uint32_t mem_sector_count(void *ctx)
{
    (void)ctx;
    return TEST_SECTORS;
}

static const odfs_media_ops_t mem_media_ops = {
    .read_sectors = mem_read_sectors,
    .sector_size  = mem_sector_size,
    .sector_count = mem_sector_count,
    .read_toc     = NULL,
    .read_audio   = NULL,
    .close        = NULL,
};

static void write_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void write_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void write_dir_record(uint8_t *rec, uint32_t extent,
                             uint32_t size, uint8_t flags,
                             const uint8_t *name, uint8_t name_len)
{
    uint8_t len = (uint8_t)(33 + name_len);

    if ((len & 1) != 0)
        len++;

    memset(rec, 0, len);
    rec[ISO_DR_LENGTH] = len;
    write_le32(&rec[ISO_DR_EXTENT_LBA], extent);
    write_le32(&rec[ISO_DR_DATA_LENGTH], size);
    rec[ISO_DR_DATE] = 126;
    rec[ISO_DR_DATE + 1] = 1;
    rec[ISO_DR_DATE + 2] = 1;
    rec[ISO_DR_FLAGS] = flags;
    write_le16(&rec[28], 1);
    rec[ISO_DR_NAME_LEN] = name_len;
    memcpy(&rec[ISO_DR_NAME], name, name_len);
}

TEST(joliet_mount_keeps_absolute_multisession_root_lba)
{
    mem_media_t media_data;
    odfs_media_t media;
    odfs_cache_t cache;
    odfs_log_state_t log;
    odfs_node_t root;
    void *ctx = NULL;
    uint8_t dot = 0;
    uint8_t dotdot = 1;
    const uint32_t session_start = 100;
    const uint32_t root_lba = 120;

    memset(&media_data, 0, sizeof(media_data));
    media.ops = &mem_media_ops;
    media.ctx = &media_data;
    odfs_log_init(&log);

    media_data.sectors[session_start + ISO_VD_START_LBA][ISO_PVD_TYPE] =
        ISO_VD_TYPE_SUPPL;
    memcpy(&media_data.sectors[session_start + ISO_VD_START_LBA][ISO_PVD_ID],
           ISO_STANDARD_ID, ISO_STANDARD_ID_LEN);
    media_data.sectors[session_start + ISO_VD_START_LBA][6] = 1;
    memcpy(&media_data.sectors[session_start + ISO_VD_START_LBA][88],
           JOLIET_ESC_UCS2_LEVEL3, 3);
    write_le32(&media_data.sectors[session_start + ISO_VD_START_LBA]
               [ISO_PVD_VOLUME_SPACE_SIZE], 40);
    write_le16(&media_data.sectors[session_start + ISO_VD_START_LBA]
               [ISO_PVD_LOGICAL_BLK_SIZE], TEST_SECTOR_SIZE);
    write_dir_record(&media_data.sectors[session_start + ISO_VD_START_LBA]
                     [ISO_PVD_ROOT_DIR_RECORD],
                     root_lba, TEST_SECTOR_SIZE, ISO_DR_FLAG_DIRECTORY,
                     &dot, 1);

    media_data.sectors[session_start + ISO_VD_START_LBA + 1][ISO_PVD_TYPE] =
        ISO_VD_TYPE_TERM;
    memcpy(&media_data.sectors[session_start + ISO_VD_START_LBA + 1]
           [ISO_PVD_ID],
           ISO_STANDARD_ID, ISO_STANDARD_ID_LEN);

    write_dir_record(&media_data.sectors[root_lba][0],
                     root_lba, TEST_SECTOR_SIZE, ISO_DR_FLAG_DIRECTORY,
                     &dot, 1);
    write_dir_record(&media_data.sectors[root_lba][34],
                     root_lba, TEST_SECTOR_SIZE, ISO_DR_FLAG_DIRECTORY,
                     &dotdot, 1);

    ASSERT_OK(odfs_cache_init(&cache, &media, 8));
    ASSERT_OK(joliet_backend_ops.mount(&cache, &log, session_start,
                                       &root, &ctx));
    ASSERT_EQ(root.extent.lba, root_lba);

    joliet_backend_ops.unmount(ctx);
    odfs_cache_destroy(&cache);
}

TEST_MAIN()
