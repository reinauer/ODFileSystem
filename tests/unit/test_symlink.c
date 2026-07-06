/*
 * test_symlink.c — Rock Ridge symlink exposure and readlink
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Builds a synthetic ISO 9660 + Rock Ridge image containing symlinks
 * (SL entries), mounts it through the core, and verifies that nodes
 * surface as ODFS_NODE_SYMLINK and that odfs_readlink() returns the
 * POSIX targets. Also covers the POSIX → AmigaDOS path conversion used
 * by the handler's ACTION_READ_LINK.
 */

#include "iso9660/iso9660.h"
#include "odfs/api.h"
#include "odfs/charset.h"
#include "odfs/error.h"
#include "test_harness.h"

#include <string.h>

#define SL_TEST_SECTORS  32
#define SL_ROOT_LBA      20
#define SL_TARGET_LBA    21

static const char sl_target_data[] = "hello\n";

typedef struct sl_media {
    uint8_t sectors[SL_TEST_SECTORS][ISO_SECTOR_SIZE];
} sl_media_t;

static odfs_err_t sl_read_sectors(void *ctx, uint32_t lba,
                                  uint32_t count, void *buf)
{
    sl_media_t *media = ctx;

    if (lba + count > SL_TEST_SECTORS)
        return ODFS_ERR_EOF;

    memcpy(buf, &media->sectors[lba][0], (size_t)count * ISO_SECTOR_SIZE);
    return ODFS_OK;
}

static uint32_t sl_sector_size(void *ctx)
{
    (void)ctx;
    return ISO_SECTOR_SIZE;
}

static uint32_t sl_sector_count(void *ctx)
{
    (void)ctx;
    return SL_TEST_SECTORS;
}

static const odfs_media_ops_t sl_media_ops = {
    .read_sectors = sl_read_sectors,
    .sector_size = sl_sector_size,
    .sector_count = sl_sector_count,
};

static void sl_wr_bb32(uint8_t *p, uint32_t v)
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

static void sl_wr_bb16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/*
 * Write an ISO 9660 directory record with an optional System Use Area.
 * Returns the (even) record length.
 */
static size_t sl_dir_record(uint8_t *p, uint32_t lba, uint32_t size,
                            uint8_t flags, const char *name, size_t name_len,
                            const uint8_t *sua, size_t sua_len)
{
    size_t name_end = 33 + name_len + ((name_len & 1) == 0 ? 1 : 0);
    size_t rec_len = name_end + sua_len;

    if (rec_len & 1)
        rec_len++; /* records must have even length */

    memset(p, 0, rec_len);
    p[0] = (uint8_t)rec_len;
    sl_wr_bb32(&p[ISO_DR_EXTENT_LBA], lba);
    sl_wr_bb32(&p[ISO_DR_DATA_LENGTH], size);
    p[ISO_DR_DATE + 0] = 95;
    p[ISO_DR_DATE + 1] = 6;
    p[ISO_DR_DATE + 2] = 1;
    p[ISO_DR_FLAGS] = flags;
    sl_wr_bb16(&p[ISO_DR_VOLUME_SEQ_NUM], 1);
    p[ISO_DR_NAME_LEN] = (uint8_t)name_len;
    memcpy(&p[ISO_DR_NAME], name, name_len);
    if (sua && sua_len > 0)
        memcpy(&p[name_end], sua, sua_len);
    return rec_len;
}

/* PX entry: RRIP 1.09 form, 36 bytes */
static size_t sl_px(uint8_t *p, uint32_t mode)
{
    memset(p, 0, 36);
    p[0] = 'P'; p[1] = 'X'; p[2] = 36; p[3] = 1;
    sl_wr_bb32(&p[4], mode);   /* st_mode */
    sl_wr_bb32(&p[12], 1);     /* st_nlink */
    return 36;
}

/* SL entry from raw component records */
static size_t sl_sl(uint8_t *p, const uint8_t *comps, size_t comps_len)
{
    p[0] = 'S'; p[1] = 'L'; p[2] = (uint8_t)(5 + comps_len); p[3] = 1;
    p[4] = 0; /* flags */
    memcpy(&p[5], comps, comps_len);
    return 5 + comps_len;
}

static void sl_build_image(sl_media_t *media)
{
    uint8_t *pvd = media->sectors[ISO_VD_START_LBA];
    uint8_t *term = media->sectors[ISO_VD_START_LBA + 1];
    uint8_t *root = media->sectors[SL_ROOT_LBA];
    uint8_t sua[128];
    size_t sua_len;
    size_t off;

    memset(media, 0, sizeof(*media));

    pvd[ISO_PVD_TYPE] = ISO_VD_TYPE_PRIMARY;
    memcpy(&pvd[ISO_PVD_ID], ISO_STANDARD_ID, ISO_STANDARD_ID_LEN);
    pvd[ISO_PVD_VERSION] = 1;
    memset(&pvd[ISO_PVD_SYSTEM_ID], ' ', 32);
    memset(&pvd[ISO_PVD_VOLUME_ID], ' ', 32);
    memcpy(&pvd[ISO_PVD_VOLUME_ID], "SLTEST", 6);
    sl_wr_bb32(&pvd[ISO_PVD_VOLUME_SPACE_SIZE], SL_TEST_SECTORS);
    sl_wr_bb16(&pvd[ISO_PVD_LOGICAL_BLK_SIZE], ISO_SECTOR_SIZE);
    sl_dir_record(&pvd[ISO_PVD_ROOT_DIR_RECORD], SL_ROOT_LBA,
                  ISO_SECTOR_SIZE, ISO_DR_FLAG_DIRECTORY, "\x00", 1,
                  NULL, 0);

    term[ISO_PVD_TYPE] = ISO_VD_TYPE_TERM;
    memcpy(&term[ISO_PVD_ID], ISO_STANDARD_ID, ISO_STANDARD_ID_LEN);
    term[ISO_PVD_VERSION] = 1;

    off = 0;

    /* "." with SUSP SP entry — announces Rock Ridge for the volume */
    {
        static const uint8_t sp[] = { 'S', 'P', 7, 1, 0xBE, 0xEF, 0 };
        off += sl_dir_record(&root[off], SL_ROOT_LBA, ISO_SECTOR_SIZE,
                             ISO_DR_FLAG_DIRECTORY, "\x00", 1,
                             sp, sizeof(sp));
    }
    off += sl_dir_record(&root[off], SL_ROOT_LBA, ISO_SECTOR_SIZE,
                         ISO_DR_FLAG_DIRECTORY, "\x01", 1, NULL, 0);

    /* plain file the links point at */
    off += sl_dir_record(&root[off], SL_TARGET_LBA,
                         sizeof(sl_target_data) - 1, 0,
                         "TARGET.TXT;1", 12, NULL, 0);

    /* LINK.TXT -> "target.txt" (single named component) */
    {
        static const uint8_t comps[] = {
            0, 10, 't', 'a', 'r', 'g', 'e', 't', '.', 't', 'x', 't'
        };
        sua_len = sl_px(sua, 0120777);
        sua_len += sl_sl(sua + sua_len, comps, sizeof(comps));
        off += sl_dir_record(&root[off], 0, 0, 0,
                             "LINK.TXT;1", 10, sua, sua_len);
    }

    /* UP.LNK -> "../target.txt" (parent component + named component) */
    {
        static const uint8_t comps[] = {
            0x04, 0,                       /* ".." */
            0, 10, 't', 'a', 'r', 'g', 'e', 't', '.', 't', 'x', 't'
        };
        sua_len = sl_px(sua, 0120777);
        sua_len += sl_sl(sua + sua_len, comps, sizeof(comps));
        off += sl_dir_record(&root[off], 0, 0, 0,
                             "UP.LNK;1", 8, sua, sua_len);
    }

    memcpy(media->sectors[SL_TARGET_LBA], sl_target_data,
           sizeof(sl_target_data) - 1);
}

static odfs_err_t sl_mount_image(sl_media_t *media, odfs_mount_t *mnt)
{
    odfs_media_t m;

    sl_build_image(media);
    m.ops = &sl_media_ops;
    m.ctx = media;
    return odfs_mount(&m, NULL, NULL, mnt);
}

typedef struct sl_collect {
    int count;
    odfs_node_t entries[8];
} sl_collect_t;

static odfs_err_t sl_collect_cb(const odfs_node_t *entry, void *ctx)
{
    sl_collect_t *c = ctx;

    if (c->count < 8)
        c->entries[c->count] = *entry;
    c->count++;
    return ODFS_OK;
}

TEST(symlink_nodes_have_symlink_kind)
{
    static sl_media_t media;
    odfs_mount_t mnt;
    sl_collect_t collect;
    int i, links = 0;

    ASSERT_OK(sl_mount_image(&media, &mnt));
    ASSERT_EQ(mnt.root.backend, ODFS_BACKEND_ROCK_RIDGE);

    memset(&collect, 0, sizeof(collect));
    ASSERT_OK(odfs_readdir(&mnt, &mnt.root, sl_collect_cb, &collect, NULL));
    ASSERT_EQ(collect.count, 3);

    for (i = 0; i < collect.count; i++) {
        if (collect.entries[i].kind == ODFS_NODE_SYMLINK)
            links++;
    }
    ASSERT_EQ(links, 2);

    odfs_unmount(&mnt);
}

TEST(readlink_returns_posix_target)
{
    static sl_media_t media;
    odfs_mount_t mnt;
    char target[256];

    ASSERT_OK(sl_mount_image(&media, &mnt));

    ASSERT_OK(odfs_readlink(&mnt, &mnt.root, "LINK.TXT",
                            target, sizeof(target)));
    ASSERT_STR_EQ(target, "target.txt");

    /* parent component followed by a name must include the separator */
    ASSERT_OK(odfs_readlink(&mnt, &mnt.root, "UP.LNK",
                            target, sizeof(target)));
    ASSERT_STR_EQ(target, "../target.txt");

    odfs_unmount(&mnt);
}

TEST(readlink_rejects_non_links)
{
    static sl_media_t media;
    odfs_mount_t mnt;
    char target[256];

    ASSERT_OK(sl_mount_image(&media, &mnt));

    ASSERT_ERR(odfs_readlink(&mnt, &mnt.root, "TARGET.TXT",
                             target, sizeof(target)),
               ODFS_ERR_UNSUPPORTED);
    ASSERT_ERR(odfs_readlink(&mnt, &mnt.root, "NOPE.TXT",
                             target, sizeof(target)),
               ODFS_ERR_NOT_FOUND);

    odfs_unmount(&mnt);
}

TEST(posix_to_amiga_conversion)
{
    char out[128];

    ASSERT_OK(odfs_posix_to_amiga_path("target.txt", out, sizeof(out)));
    ASSERT_STR_EQ(out, "target.txt");

    ASSERT_OK(odfs_posix_to_amiga_path("a/b/c", out, sizeof(out)));
    ASSERT_STR_EQ(out, "a/b/c");

    ASSERT_OK(odfs_posix_to_amiga_path("../x", out, sizeof(out)));
    ASSERT_STR_EQ(out, "/x");

    ASSERT_OK(odfs_posix_to_amiga_path("../../x", out, sizeof(out)));
    ASSERT_STR_EQ(out, "//x");

    ASSERT_OK(odfs_posix_to_amiga_path("a/../b", out, sizeof(out)));
    ASSERT_STR_EQ(out, "b");

    ASSERT_OK(odfs_posix_to_amiga_path("./a/./b", out, sizeof(out)));
    ASSERT_STR_EQ(out, "a/b");

    ASSERT_OK(odfs_posix_to_amiga_path("/usr/lib", out, sizeof(out)));
    ASSERT_STR_EQ(out, ":usr/lib");

    ASSERT_OK(odfs_posix_to_amiga_path("/", out, sizeof(out)));
    ASSERT_STR_EQ(out, ":");

    ASSERT_OK(odfs_posix_to_amiga_path(".", out, sizeof(out)));
    ASSERT_STR_EQ(out, "");

    ASSERT_OK(odfs_posix_to_amiga_path("..", out, sizeof(out)));
    ASSERT_STR_EQ(out, "/");

    /* ".." above an absolute root is dropped */
    ASSERT_OK(odfs_posix_to_amiga_path("/../x", out, sizeof(out)));
    ASSERT_STR_EQ(out, ":x");
}

TEST_MAIN()
