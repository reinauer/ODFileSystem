/*
 * test_multisession_mount.c — tests for multisession mount fallback
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "iso9660/iso9660.h"
#include "odfs/api.h"
#include "odfs/error.h"
#include "test_harness.h"

#include <string.h>

#define TEST_SECTOR_SIZE 2048
#define TEST_SECTORS     256

typedef struct multisession_media {
    uint8_t    sectors[TEST_SECTORS][TEST_SECTOR_SIZE];
    odfs_toc_t toc;
    uint32_t   last_session_lba;
} multisession_media_t;

static odfs_err_t multisession_read_sectors(void *ctx, uint32_t lba,
                                            uint32_t count, void *buf)
{
    multisession_media_t *media = ctx;

    if (lba + count > TEST_SECTORS)
        return ODFS_ERR_EOF;

    memcpy(buf, &media->sectors[lba][0], (size_t)count * TEST_SECTOR_SIZE);
    return ODFS_OK;
}

static uint32_t multisession_sector_size(void *ctx)
{
    (void)ctx;
    return TEST_SECTOR_SIZE;
}

static uint32_t multisession_sector_count(void *ctx)
{
    (void)ctx;
    return TEST_SECTORS;
}

static odfs_err_t multisession_read_toc(void *ctx, odfs_toc_t *toc)
{
    multisession_media_t *media = ctx;

    *toc = media->toc;
    return ODFS_OK;
}

static odfs_err_t multisession_read_last_session_lba(void *ctx,
                                                     uint32_t *lba_out)
{
    multisession_media_t *media = ctx;

    *lba_out = media->last_session_lba;
    return ODFS_OK;
}

static const odfs_media_ops_t multisession_media_ops = {
    .read_sectors          = multisession_read_sectors,
    .sector_size           = multisession_sector_size,
    .sector_count          = multisession_sector_count,
    .read_toc              = multisession_read_toc,
    .read_last_session_lba = multisession_read_last_session_lba,
    .read_audio            = NULL,
    .close                 = NULL,
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

static void build_iso_session(multisession_media_t *media,
                              uint32_t session_start,
                              const char *volume_name)
{
    uint8_t dot = 0;
    uint8_t dotdot = 1;
    uint32_t pvd_lba = session_start + ISO_VD_START_LBA;
    uint32_t root_lba = session_start + 20;
    size_t volume_len = strlen(volume_name);

    media->sectors[pvd_lba][ISO_PVD_TYPE] = ISO_VD_TYPE_PRIMARY;
    memcpy(&media->sectors[pvd_lba][ISO_PVD_ID],
           ISO_STANDARD_ID, ISO_STANDARD_ID_LEN);
    media->sectors[pvd_lba][6] = 1;

    memset(&media->sectors[pvd_lba][ISO_PVD_VOLUME_ID], ' ', 32);
    if (volume_len > 32)
        volume_len = 32;
    memcpy(&media->sectors[pvd_lba][ISO_PVD_VOLUME_ID],
           volume_name, volume_len);

    write_le32(&media->sectors[pvd_lba][ISO_PVD_VOLUME_SPACE_SIZE], 40);
    write_le16(&media->sectors[pvd_lba][ISO_PVD_LOGICAL_BLK_SIZE],
               TEST_SECTOR_SIZE);
    write_dir_record(&media->sectors[pvd_lba][ISO_PVD_ROOT_DIR_RECORD],
                     root_lba, TEST_SECTOR_SIZE, ISO_DR_FLAG_DIRECTORY,
                     &dot, 1);

    media->sectors[pvd_lba + 1][ISO_PVD_TYPE] = ISO_VD_TYPE_TERM;
    memcpy(&media->sectors[pvd_lba + 1][ISO_PVD_ID],
           ISO_STANDARD_ID, ISO_STANDARD_ID_LEN);

    write_dir_record(&media->sectors[root_lba][0],
                     root_lba, TEST_SECTOR_SIZE, ISO_DR_FLAG_DIRECTORY,
                     &dot, 1);
    write_dir_record(&media->sectors[root_lba][34],
                     root_lba, TEST_SECTOR_SIZE, ISO_DR_FLAG_DIRECTORY,
                     &dotdot, 1);
}

static void init_photo_cd_like_toc(multisession_media_t *media,
                                   uint32_t last_session_lba)
{
    memset(&media->toc, 0, sizeof(media->toc));
    media->toc.session_count = 2;
    media->toc.sessions[0].number = 1;
    media->toc.sessions[0].control = 0x04;
    media->toc.sessions[0].start_lba = 0;
    media->toc.sessions[0].length = 100;
    media->toc.sessions[1].number = 2;
    media->toc.sessions[1].control = 0x04;
    media->toc.sessions[1].start_lba = 100;
    media->toc.sessions[1].length = 100;
    media->last_session_lba = last_session_lba;
}

TEST(mount_falls_back_to_earlier_data_session_when_last_has_no_fs)
{
    multisession_media_t media_data;
    odfs_media_t media;
    odfs_mount_t mount;
    odfs_mount_opts_t opts;

    memset(&media_data, 0, sizeof(media_data));
    build_iso_session(&media_data, 0, "TRACK1");
    init_photo_cd_like_toc(&media_data, 100);

    media.ops = &multisession_media_ops;
    media.ctx = &media_data;
    odfs_mount_opts_default(&opts);

    ASSERT_OK(odfs_mount(&media, &opts, NULL, &mount));
    ASSERT_STR_EQ(mount.volume_name, "TRACK1");
    ASSERT_EQ(mount.root.extent.lba, 20);

    odfs_unmount(&mount);
}

TEST(mount_keeps_later_data_session_when_it_is_mountable)
{
    multisession_media_t media_data;
    odfs_media_t media;
    odfs_mount_t mount;
    odfs_mount_opts_t opts;

    memset(&media_data, 0, sizeof(media_data));
    build_iso_session(&media_data, 0, "TRACK1");
    build_iso_session(&media_data, 100, "TRACK2");
    init_photo_cd_like_toc(&media_data, 100);

    media.ops = &multisession_media_ops;
    media.ctx = &media_data;
    odfs_mount_opts_default(&opts);

    ASSERT_OK(odfs_mount(&media, &opts, NULL, &mount));
    ASSERT_STR_EQ(mount.volume_name, "TRACK2");
    ASSERT_EQ(mount.root.extent.lba, 120);

    odfs_unmount(&mount);
}

TEST_MAIN()
