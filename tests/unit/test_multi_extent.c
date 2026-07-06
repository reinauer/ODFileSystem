/*
 * test_multi_extent.c — ISO 9660 Level 3 multi-extent file merging
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Builds a synthetic ISO 9660 image containing a file split across two
 * contiguous extents (as mkisofs -iso-level 3 writes them), a file whose
 * second extent is non-contiguous (unrepresentable in the single-extent
 * node model — must be truncated with a warning, not mis-read), and a
 * normal file after both to prove the directory stream stays intact.
 */

#include "iso9660/iso9660.h"
#include "odfs/api.h"
#include "odfs/error.h"
#include "test_harness.h"

#include <string.h>

#define ME_TEST_SECTORS  32
#define ME_ROOT_LBA      20
#define ME_BIG_LBA       21    /* extents: 21-22 (4096) + 23 (100) */
#define ME_BIG_PART2_LBA 23
#define ME_SPLIT_LBA     24    /* extents: 24 (2048) + 26 (50), gap at 25 */
#define ME_SPLIT_PART2_LBA 26
#define ME_TAIL_LBA      27

#define ME_BIG_PART1_LEN 4096
#define ME_BIG_PART2_LEN 100
#define ME_SPLIT_PART1_LEN 2048
#define ME_SPLIT_PART2_LEN 50

static const char me_tail_data[] = "tail";

typedef struct me_media {
    uint8_t sectors[ME_TEST_SECTORS][ISO_SECTOR_SIZE];
} me_media_t;

static odfs_err_t me_read_sectors(void *ctx, uint32_t lba,
                                  uint32_t count, void *buf)
{
    me_media_t *media = ctx;

    if (lba + count > ME_TEST_SECTORS)
        return ODFS_ERR_EOF;

    memcpy(buf, &media->sectors[lba][0], (size_t)count * ISO_SECTOR_SIZE);
    return ODFS_OK;
}

static uint32_t me_sector_size(void *ctx)
{
    (void)ctx;
    return ISO_SECTOR_SIZE;
}

static uint32_t me_sector_count(void *ctx)
{
    (void)ctx;
    return ME_TEST_SECTORS;
}

static const odfs_media_ops_t me_media_ops = {
    .read_sectors = me_read_sectors,
    .sector_size = me_sector_size,
    .sector_count = me_sector_count,
};

static void me_wr_bb32(uint8_t *p, uint32_t v)
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

static void me_wr_bb16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* write an ISO 9660 directory record (7-byte date, flags at offset 25) */
static size_t me_dir_record(uint8_t *p, uint32_t lba, uint32_t size,
                            uint8_t flags, const char *name, size_t name_len)
{
    size_t rec_len = 33 + name_len + ((name_len & 1) == 0 ? 1 : 0);

    memset(p, 0, rec_len);
    p[0] = (uint8_t)rec_len;
    me_wr_bb32(&p[ISO_DR_EXTENT_LBA], lba);
    me_wr_bb32(&p[ISO_DR_DATA_LENGTH], size);
    p[ISO_DR_DATE + 0] = 95;   /* 1995 */
    p[ISO_DR_DATE + 1] = 6;
    p[ISO_DR_DATE + 2] = 1;
    p[ISO_DR_FLAGS] = flags;
    me_wr_bb16(&p[ISO_DR_VOLUME_SEQ_NUM], 1);
    p[ISO_DR_NAME_LEN] = (uint8_t)name_len;
    memcpy(&p[ISO_DR_NAME], name, name_len);
    return rec_len;
}

static void me_build_image(me_media_t *media)
{
    uint8_t *pvd = media->sectors[ISO_VD_START_LBA];
    uint8_t *term = media->sectors[ISO_VD_START_LBA + 1];
    uint8_t *root = media->sectors[ME_ROOT_LBA];
    size_t off;

    memset(media, 0, sizeof(*media));

    /* --- primary volume descriptor at sector 16 --- */
    pvd[ISO_PVD_TYPE] = ISO_VD_TYPE_PRIMARY;
    memcpy(&pvd[ISO_PVD_ID], ISO_STANDARD_ID, ISO_STANDARD_ID_LEN);
    pvd[ISO_PVD_VERSION] = 1;
    memset(&pvd[ISO_PVD_SYSTEM_ID], ' ', 32);
    memset(&pvd[ISO_PVD_VOLUME_ID], ' ', 32);
    memcpy(&pvd[ISO_PVD_VOLUME_ID], "METEST", 6);
    me_wr_bb32(&pvd[ISO_PVD_VOLUME_SPACE_SIZE], ME_TEST_SECTORS);
    me_wr_bb16(&pvd[ISO_PVD_LOGICAL_BLK_SIZE], ISO_SECTOR_SIZE);
    me_dir_record(&pvd[ISO_PVD_ROOT_DIR_RECORD], ME_ROOT_LBA,
                  ISO_SECTOR_SIZE, ISO_DR_FLAG_DIRECTORY, "\x00", 1);

    /* --- volume descriptor set terminator at sector 17 --- */
    term[ISO_PVD_TYPE] = ISO_VD_TYPE_TERM;
    memcpy(&term[ISO_PVD_ID], ISO_STANDARD_ID, ISO_STANDARD_ID_LEN);
    term[ISO_PVD_VERSION] = 1;

    /* --- root directory extent --- */
    off = 0;
    off += me_dir_record(&root[off], ME_ROOT_LBA, ISO_SECTOR_SIZE,
                         ISO_DR_FLAG_DIRECTORY, "\x00", 1);
    off += me_dir_record(&root[off], ME_ROOT_LBA, ISO_SECTOR_SIZE,
                         ISO_DR_FLAG_DIRECTORY, "\x01", 1);

    /* BIG.DAT: two contiguous extents, second is the final one */
    off += me_dir_record(&root[off], ME_BIG_LBA, ME_BIG_PART1_LEN,
                         ISO_DR_FLAG_MULTI_EXTENT, "BIG.DAT;1", 9);
    off += me_dir_record(&root[off], ME_BIG_PART2_LBA, ME_BIG_PART2_LEN,
                         0, "BIG.DAT;1", 9);

    /* SPLIT.DAT: second extent leaves a one-sector gap */
    off += me_dir_record(&root[off], ME_SPLIT_LBA, ME_SPLIT_PART1_LEN,
                         ISO_DR_FLAG_MULTI_EXTENT, "SPLIT.DAT;1", 11);
    off += me_dir_record(&root[off], ME_SPLIT_PART2_LBA, ME_SPLIT_PART2_LEN,
                         0, "SPLIT.DAT;1", 11);

    /* a normal file after the multi-extent entries */
    off += me_dir_record(&root[off], ME_TAIL_LBA,
                         sizeof(me_tail_data) - 1, 0, "TAIL.TXT;1", 10);

    /* --- file data --- */
    memset(media->sectors[ME_BIG_LBA], 'A', ISO_SECTOR_SIZE);
    memset(media->sectors[ME_BIG_LBA + 1], 'B', ISO_SECTOR_SIZE);
    memset(media->sectors[ME_BIG_PART2_LBA], 'C', ME_BIG_PART2_LEN);
    memset(media->sectors[ME_SPLIT_LBA], 'D', ME_SPLIT_PART1_LEN);
    memset(media->sectors[ME_SPLIT_PART2_LBA], 'E', ME_SPLIT_PART2_LEN);
    memcpy(media->sectors[ME_TAIL_LBA], me_tail_data,
           sizeof(me_tail_data) - 1);
}

static odfs_err_t me_mount_image(me_media_t *media, odfs_mount_t *mnt)
{
    odfs_media_t m;

    me_build_image(media);
    m.ops = &me_media_ops;
    m.ctx = media;
    return odfs_mount(&m, NULL, NULL, mnt);
}

typedef struct me_collect {
    int count;
    odfs_node_t entries[8];
} me_collect_t;

static odfs_err_t me_collect_cb(const odfs_node_t *entry, void *ctx)
{
    me_collect_t *c = ctx;

    if (c->count < 8)
        c->entries[c->count] = *entry;
    c->count++;
    return ODFS_OK;
}

TEST(multi_extent_merges_contiguous_parts)
{
    static me_media_t media;
    odfs_mount_t mnt;
    odfs_node_t file;

    ASSERT_OK(me_mount_image(&media, &mnt));

    ASSERT_OK(odfs_resolve_path(&mnt, "BIG.DAT", &file));
    ASSERT_EQ(file.kind, ODFS_NODE_FILE);
    ASSERT_EQ(file.size, ME_BIG_PART1_LEN + ME_BIG_PART2_LEN);
    ASSERT_EQ(file.extent.lba, ME_BIG_LBA);

    odfs_unmount(&mnt);
}

TEST(multi_extent_read_crosses_extent_boundary)
{
    static me_media_t media;
    odfs_mount_t mnt;
    odfs_node_t file;
    char buf[128];
    size_t len;
    int i;

    ASSERT_OK(me_mount_image(&media, &mnt));
    ASSERT_OK(odfs_resolve_path(&mnt, "BIG.DAT", &file));

    /* read the last 6 bytes of part 1 and all 100 bytes of part 2 */
    memset(buf, 0, sizeof(buf));
    len = 106;
    ASSERT_OK(odfs_read(&mnt, &file, ME_BIG_PART1_LEN - 6, buf, &len));
    ASSERT_EQ(len, 106);
    for (i = 0; i < 6; i++)
        ASSERT_EQ(buf[i], 'B');
    for (i = 6; i < 106; i++)
        ASSERT_EQ(buf[i], 'C');

    /* a read past the merged size is clamped */
    len = sizeof(buf);
    ASSERT_OK(odfs_read(&mnt, &file, file.size - 4, buf, &len));
    ASSERT_EQ(len, 4);

    odfs_unmount(&mnt);
}

TEST(multi_extent_no_duplicate_entries)
{
    static me_media_t media;
    odfs_mount_t mnt;
    me_collect_t collect;

    ASSERT_OK(me_mount_image(&media, &mnt));

    memset(&collect, 0, sizeof(collect));
    ASSERT_OK(odfs_readdir(&mnt, &mnt.root, me_collect_cb, &collect, NULL));

    /* continuation records must not surface as directory entries */
    ASSERT_EQ(collect.count, 3);
    ASSERT_STR_EQ(collect.entries[0].name, "BIG.DAT");
    ASSERT_STR_EQ(collect.entries[1].name, "SPLIT.DAT");
    ASSERT_STR_EQ(collect.entries[2].name, "TAIL.TXT");

    odfs_unmount(&mnt);
}

TEST(multi_extent_noncontiguous_truncates)
{
    static me_media_t media;
    odfs_mount_t mnt;
    odfs_node_t file;
    char buf[16];
    size_t len;

    ASSERT_OK(me_mount_image(&media, &mnt));

    /* the gap makes part 2 unreachable through a single extent — the
     * file must be truncated to the contiguous prefix, never mis-read */
    ASSERT_OK(odfs_resolve_path(&mnt, "SPLIT.DAT", &file));
    ASSERT_EQ(file.size, ME_SPLIT_PART1_LEN);

    len = sizeof(buf);
    ASSERT_OK(odfs_read(&mnt, &file, ME_SPLIT_PART1_LEN - 2, buf, &len));
    ASSERT_EQ(len, 2);
    ASSERT_EQ(buf[0], 'D');
    ASSERT_EQ(buf[1], 'D');

    odfs_unmount(&mnt);
}

TEST(multi_extent_stream_continues_after_merge)
{
    static me_media_t media;
    odfs_mount_t mnt;
    odfs_node_t file;
    char buf[8];
    size_t len;

    ASSERT_OK(me_mount_image(&media, &mnt));

    ASSERT_OK(odfs_resolve_path(&mnt, "TAIL.TXT", &file));
    ASSERT_EQ(file.size, sizeof(me_tail_data) - 1);

    memset(buf, 0, sizeof(buf));
    len = sizeof(buf);
    ASSERT_OK(odfs_read(&mnt, &file, 0, buf, &len));
    ASSERT_EQ(len, sizeof(me_tail_data) - 1);
    ASSERT(memcmp(buf, me_tail_data, len) == 0);

    odfs_unmount(&mnt);
}

TEST_MAIN()
