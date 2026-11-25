/*
 * test_joliet.c — tests for Joliet backend edge cases
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "joliet/joliet.h"
#include "odfs/cache.h"
#include "odfs/charset.h"
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

static size_t write_dir_record(uint8_t *rec, uint32_t extent,
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
    return len;
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

typedef struct joliet_collect {
    int         count;
    odfs_node_t nodes[8];
} joliet_collect_t;

static odfs_err_t joliet_collect_cb(const odfs_node_t *entry, void *arg)
{
    joliet_collect_t *c = arg;
    if (c->count < 8)
        c->nodes[c->count] = *entry;
    c->count++;
    return ODFS_OK;
}

TEST(joliet_skips_odd_names_and_strips_version_suffix)
{
    static mem_media_t media_data;
    odfs_media_t media;
    odfs_cache_t cache;
    odfs_log_state_t log;
    odfs_node_t root;
    void *ctx = NULL;
    joliet_collect_t collect;
    uint8_t dot = 0, dotdot = 1;
    const uint32_t root_lba = 20;
    uint8_t *vd = media_data.sectors[ISO_VD_START_LBA];
    size_t off = 0;

    /* odd-length UCS-2 identifier: not a valid name */
    static const uint8_t odd_ucs2[] = { 0x00, 'X', 0x00, 'Y', 0x00 };
    /* a semicolon that does not start a version number */
    static const uint8_t semi_nonver[] = { 0x00, 'a', 0x00, ';', 0x00, 'b' };
    /* multi-digit version number "doc.txt;12" */
    static const uint8_t multi_ver[] = {
        0x00, 'd', 0x00, 'o', 0x00, 'c', 0x00, '.',
        0x00, 't', 0x00, 'x', 0x00, 't', 0x00, ';', 0x00, '1', 0x00, '2'
    };

    memset(&media_data, 0, sizeof(media_data));
    media.ops = &mem_media_ops;
    media.ctx = &media_data;
    odfs_log_init(&log);

    vd[ISO_PVD_TYPE] = ISO_VD_TYPE_SUPPL;
    memcpy(&vd[ISO_PVD_ID], ISO_STANDARD_ID, ISO_STANDARD_ID_LEN);
    vd[6] = 1;
    memcpy(&vd[88], JOLIET_ESC_UCS2_LEVEL3, 3);
    write_le32(&vd[ISO_PVD_VOLUME_SPACE_SIZE], 40);
    write_le16(&vd[ISO_PVD_LOGICAL_BLK_SIZE], TEST_SECTOR_SIZE);
    write_dir_record(&vd[ISO_PVD_ROOT_DIR_RECORD], root_lba,
                     TEST_SECTOR_SIZE, ISO_DR_FLAG_DIRECTORY, &dot, 1);

    media_data.sectors[ISO_VD_START_LBA + 1][ISO_PVD_TYPE] = ISO_VD_TYPE_TERM;
    memcpy(&media_data.sectors[ISO_VD_START_LBA + 1][ISO_PVD_ID],
           ISO_STANDARD_ID, ISO_STANDARD_ID_LEN);

    off += write_dir_record(&media_data.sectors[root_lba][off],
                            root_lba, TEST_SECTOR_SIZE, ISO_DR_FLAG_DIRECTORY,
                            &dot, 1);
    off += write_dir_record(&media_data.sectors[root_lba][off],
                            root_lba, TEST_SECTOR_SIZE, ISO_DR_FLAG_DIRECTORY,
                            &dotdot, 1);
    off += write_dir_record(&media_data.sectors[root_lba][off],
                            21, 10, 0, odd_ucs2, sizeof(odd_ucs2));
    off += write_dir_record(&media_data.sectors[root_lba][off],
                            22, 10, 0, semi_nonver, sizeof(semi_nonver));
    off += write_dir_record(&media_data.sectors[root_lba][off],
                            23, 10, 0, multi_ver, sizeof(multi_ver));

    ASSERT_OK(odfs_cache_init(&cache, &media, 8));
    ASSERT_OK(joliet_backend_ops.mount(&cache, &log, 0, &root, &ctx));

    memset(&collect, 0, sizeof(collect));
    ASSERT_OK(joliet_backend_ops.readdir(ctx, &cache, &log, &root,
                                         joliet_collect_cb, &collect, NULL));
    ASSERT_EQ(collect.count, 2);
    ASSERT_STR_EQ(collect.nodes[0].name, "a;b");
    ASSERT_STR_EQ(collect.nodes[1].name, "doc.txt");

    joliet_backend_ops.unmount(ctx);
    odfs_cache_destroy(&cache);
}

/* a Joliet tree read for a mount that wants Latin-1 names */
TEST(joliet_names_in_latin1)
{
    static mem_media_t media_data;
    odfs_media_t media;
    odfs_cache_t cache;
    odfs_log_state_t log;
    odfs_node_t root;
    odfs_node_t node;
    void *ctx = NULL;
    joliet_collect_t collect;
    uint8_t dot = 0, dotdot = 1;
    const uint32_t root_lba = 20;
    uint8_t *vd = media_data.sectors[ISO_VD_START_LBA];
    size_t off = 0;

    /* "Gr" U+00FC U+00DF "e" */
    static const uint8_t gruesse[] = {
        0x00, 'G', 0x00, 'r', 0x00, 0xFC, 0x00, 0xDF, 0x00, 'e'
    };
    /* U+65E5 U+672C and U+4E2D U+56FD: not in Latin-1 */
    static const uint8_t nihon[] = { 0x65, 0xE5, 0x67, 0x2C };
    static const uint8_t chugoku[] = { 0x4E, 0x2D, 0x56, 0xFD };
    static const uint8_t plain[] = { 0x00, 'a', 0x00, 'b', 0x00, 'c' };

    memset(&media_data, 0, sizeof(media_data));
    media.ops = &mem_media_ops;
    media.ctx = &media_data;
    odfs_log_init(&log);

    vd[ISO_PVD_TYPE] = ISO_VD_TYPE_SUPPL;
    memcpy(&vd[ISO_PVD_ID], ISO_STANDARD_ID, ISO_STANDARD_ID_LEN);
    vd[6] = 1;
    memcpy(&vd[88], JOLIET_ESC_UCS2_LEVEL3, 3);
    write_le32(&vd[ISO_PVD_VOLUME_SPACE_SIZE], 40);
    write_le16(&vd[ISO_PVD_LOGICAL_BLK_SIZE], TEST_SECTOR_SIZE);
    write_dir_record(&vd[ISO_PVD_ROOT_DIR_RECORD], root_lba,
                     TEST_SECTOR_SIZE, ISO_DR_FLAG_DIRECTORY, &dot, 1);

    media_data.sectors[ISO_VD_START_LBA + 1][ISO_PVD_TYPE] = ISO_VD_TYPE_TERM;
    memcpy(&media_data.sectors[ISO_VD_START_LBA + 1][ISO_PVD_ID],
           ISO_STANDARD_ID, ISO_STANDARD_ID_LEN);

    off += write_dir_record(&media_data.sectors[root_lba][off],
                            root_lba, TEST_SECTOR_SIZE, ISO_DR_FLAG_DIRECTORY,
                            &dot, 1);
    off += write_dir_record(&media_data.sectors[root_lba][off],
                            root_lba, TEST_SECTOR_SIZE, ISO_DR_FLAG_DIRECTORY,
                            &dotdot, 1);
    off += write_dir_record(&media_data.sectors[root_lba][off],
                            21, 10, 0, gruesse, sizeof(gruesse));
    off += write_dir_record(&media_data.sectors[root_lba][off],
                            22, 10, 0, nihon, sizeof(nihon));
    off += write_dir_record(&media_data.sectors[root_lba][off],
                            23, 10, 0, chugoku, sizeof(chugoku));
    off += write_dir_record(&media_data.sectors[root_lba][off],
                            24, 10, 0, plain, sizeof(plain));

    ASSERT_OK(odfs_cache_init(&cache, &media, 8));
    ASSERT_OK(joliet_backend_ops.mount(&cache, &log, 0, &root, &ctx));

    /* as decoded: UTF-8 */
    memset(&collect, 0, sizeof(collect));
    ASSERT_OK(joliet_backend_ops.readdir(ctx, &cache, &log, &root,
                                         joliet_collect_cb, &collect, NULL));
    ASSERT_EQ(collect.count, 4);
    ASSERT_STR_EQ(collect.nodes[0].name, "Gr\xC3\xBC\xC3\x9F" "e");
    ASSERT_STR_EQ(collect.nodes[1].name, "\xE6\x97\xA5\xE6\x9C\xAC");

    cache.names.charset = ODFS_NAMES_LATIN1;
    memset(&collect, 0, sizeof(collect));
    ASSERT_OK(joliet_backend_ops.readdir(ctx, &cache, &log, &root,
                                         joliet_collect_cb, &collect, NULL));
    ASSERT_EQ(collect.count, 4);
    ASSERT_STR_EQ(collect.nodes[0].name, "Gr\xFC\xDF" "e");
    ASSERT_STR_EQ(collect.nodes[1].name, "__");
    ASSERT_STR_EQ(collect.nodes[2].name, "__~2");
    ASSERT_STR_EQ(collect.nodes[3].name, "abc");

    /* a name is found the way readdir spelled it, and only that way */
    ASSERT_OK(joliet_backend_ops.lookup(ctx, &cache, &log, &root,
                                        "Gr\xFC\xDF" "e", &node));
    ASSERT_EQ(node.extent.lba, 21);
    ASSERT_OK(joliet_backend_ops.lookup(ctx, &cache, &log, &root,
                                        "__~2", &node));
    ASSERT_EQ(node.extent.lba, 23);
    ASSERT_ERR(joliet_backend_ops.lookup(ctx, &cache, &log, &root,
                                         "Gr\xC3\xBC\xC3\x9F" "e", &node),
               ODFS_ERR_NOT_FOUND);

    joliet_backend_ops.unmount(ctx);
    odfs_cache_destroy(&cache);
}

TEST_MAIN()
