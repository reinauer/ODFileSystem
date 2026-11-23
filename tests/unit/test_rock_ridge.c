/*
 * test_rock_ridge.c — tests for Rock Ridge parser
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "rock_ridge/rock_ridge.h"
#include "iso9660/iso9660.h"
#include "odfs/ancestry.h"
#include "odfs/api.h"
#include "odfs/cache.h"
#include "test_harness.h"

#include <string.h>

static void wr_bb16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void wr_bb32(uint8_t *p, uint32_t v)
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

TEST(rr_parse_sl_basic)
{
    const uint8_t sua[] = {
        'S', 'L', 16, 1, 0,
        0, 3, 'e', 't', 'c',
        0, 4, 'h', 'o', 's', 't',
        'S', 'T', 4, 1
    };
    rr_info_t info;
    char target[64];

    rr_parse(sua, sizeof(sua), 0, 0, &info, target, sizeof(target), NULL);

    ASSERT_EQ(info.is_symlink, 1);
    ASSERT_EQ(info.sl_truncated, 0);
    ASSERT_STR_EQ(target, "etc/host");
}

TEST(rr_parse_sl_truncated_component)
{
    const uint8_t sua[] = {
        'S', 'L', 8, 1, 0,
        0, 10, 'x',
        'S', 'T', 4, 1
    };
    rr_info_t info;
    char target[64];

    rr_parse(sua, sizeof(sua), 0, 0, &info, target, sizeof(target), NULL);

    ASSERT_EQ(info.is_symlink, 1);
    ASSERT_STR_EQ(target, "");
}

TEST(rr_parse_sl_component_continue_and_overflow)
{
    const uint8_t sua[] = {
        'S', 'L', 11, 1, 0x01,
        0x01, 4, 'v', 'e', 'r', 'y',     /* component continues */
        'S', 'L', 17, 1, 0x00,
        0x00, 4, 'l', 'o', 'n', 'g',     /* rest of the same component */
        0x00, 4, 'l', 'e', 'a', 'f',     /* next path component */
        'S', 'T', 4, 1
    };
    rr_info_t info;
    char target[64];
    char tiny[8];

    rr_parse(sua, sizeof(sua), 0, 0, &info, target, sizeof(target), NULL);
    ASSERT_EQ(info.is_symlink, 1);
    ASSERT_EQ(info.sl_truncated, 0);
    ASSERT_STR_EQ(target, "verylong/leaf");

    rr_parse(sua, sizeof(sua), 0, 0, &info, tiny, sizeof(tiny), NULL);
    ASSERT_EQ(info.is_symlink, 1);
    ASSERT_EQ(info.sl_truncated, 1);
}

TEST(rr_parse_as_basic)
{
    const uint8_t sua[] = {
        'A', 'S', 21, 1, 0x03,
        0x00, 0x00, 0x00, 0x40,
        12, 'B', 'o', 'o', 't', ' ', 's', 'c', 'r', 'i', 'p', 't',
        'S', 'T', 4, 1
    };
    rr_info_t info;

    rr_parse(sua, sizeof(sua), 0, 0, &info, NULL, 0, NULL);

    ASSERT_EQ(info.has_amiga_protection, 1);
    ASSERT_EQ(info.amiga_protection[0], 0x00);
    ASSERT_EQ(info.amiga_protection[1], 0x00);
    ASSERT_EQ(info.amiga_protection[2], 0x00);
    ASSERT_EQ(info.amiga_protection[3], 0x40);
    ASSERT_EQ(info.has_amiga_comment, 1);
    ASSERT_STR_EQ(info.amiga_comment, "Boot script");
}

TEST(rr_parse_as_comment_continue)
{
    const uint8_t sua[] = {
        'A', 'S', 13, 1, 0x07,
        0x00, 0x00, 0x00, 0x10,
        4, 'A', 'm', 'i',
        'A', 'S', 13, 1, 0x02,
        8, 'g', 'a', ' ', 'R', 'R', 'I', 'P',
        'S', 'T', 4, 1
    };
    rr_info_t info;

    rr_parse(sua, sizeof(sua), 0, 0, &info, NULL, 0, NULL);

    ASSERT_EQ(info.has_amiga_protection, 1);
    ASSERT_EQ(info.amiga_protection[3], 0x10);
    ASSERT_EQ(info.has_amiga_comment, 1);
    ASSERT_STR_EQ(info.amiga_comment, "Amiga RRIP");
}

TEST(rr_parse_tf_short_entry_and_single_timestamps)
{
    /* a 4-byte TF entry has no flags byte; the bytes after it are not TF */
    const uint8_t short_tf[] = {
        'T', 'F', 4, 1,
        RR_TF_MODIFY, 126, 5, 19, 14, 30, 15, 4
    };
    const uint8_t tf7[] = {
        'T', 'F', 12, 1, RR_TF_MODIFY,
        126, 5, 19, 14, 30, 15, 4
    };
    const uint8_t tf17[] = {
        'T', 'F', 22, 1, RR_TF_CREATION | RR_TF_LONG_FORM,
        '2', '0', '2', '6', '1', '1', '2', '1',
        '0', '9', '4', '5', '3', '3', '0', '0', (uint8_t)-4
    };
    rr_info_t info;

    rr_parse(short_tf, 4, 0, 0, &info, NULL, 0, NULL);
    ASSERT_EQ(info.has_mtime, 0);
    ASSERT_EQ(info.has_ctime, 0);

    rr_parse(tf7, sizeof(tf7), 0, 0, &info, NULL, 0, NULL);
    ASSERT_EQ(info.has_mtime, 1);
    ASSERT_EQ(info.has_ctime, 0);
    ASSERT_EQ(info.mtime.year, 2026);
    ASSERT_EQ(info.mtime.month, 5);
    ASSERT_EQ(info.mtime.day, 19);
    ASSERT_EQ(info.mtime.tz_offset, 60);

    rr_parse(tf17, sizeof(tf17), 0, 0, &info, NULL, 0, NULL);
    ASSERT_EQ(info.has_mtime, 0);
    ASSERT_EQ(info.has_ctime, 1);
    ASSERT_EQ(info.ctime.year, 2026);
    ASSERT_EQ(info.ctime.month, 11);
    ASSERT_EQ(info.ctime.day, 21);
    ASSERT_EQ(info.ctime.second, 33);
    ASSERT_EQ(info.ctime.tz_offset, -60);
}

/* ---- synthetic media for CE and relocated directory tests ---- */

#define RR_TEST_SECTORS 64

typedef struct rr_media {
    uint8_t sectors[RR_TEST_SECTORS][ISO_SECTOR_SIZE];
} rr_media_t;

static odfs_err_t rr_read_sectors(void *ctx, uint32_t lba,
                                  uint32_t count, void *buf)
{
    rr_media_t *m = ctx;
    if (lba >= RR_TEST_SECTORS || count > RR_TEST_SECTORS - lba)
        return ODFS_ERR_EOF;
    memcpy(buf, &m->sectors[lba][0], (size_t)count * ISO_SECTOR_SIZE);
    return ODFS_OK;
}

static uint32_t rr_sector_size(void *ctx)
{
    (void)ctx;
    return ISO_SECTOR_SIZE;
}

static uint32_t rr_sector_count(void *ctx)
{
    (void)ctx;
    return RR_TEST_SECTORS;
}

static const odfs_media_ops_t rr_media_ops = {
    .read_sectors = rr_read_sectors,
    .sector_size  = rr_sector_size,
    .sector_count = rr_sector_count,
};

static size_t rr_put_ce(uint8_t *p, uint32_t lba, uint32_t off, uint32_t len)
{
    memset(p, 0, 28);
    p[0] = 'C'; p[1] = 'E'; p[2] = 28; p[3] = 1;
    wr_bb32(&p[4], lba);
    wr_bb32(&p[12], off);
    wr_bb32(&p[20], len);
    return 28;
}

TEST(rr_parse_ce_continues_name_in_session_and_finishes_area)
{
    static rr_media_t media_data;
    odfs_media_t media;
    odfs_cache_t cache;
    const uint8_t *sec;
    rr_info_t info;
    const uint32_t session_start = 40;
    const uint32_t ce_rel_lba = 5;
    uint8_t *s = media_data.sectors[10];
    uint8_t *ce = &media_data.sectors[session_start + ce_rel_lba][64];
    size_t p = 0;

    memset(&media_data, 0, sizeof(media_data));
    media.ops = &rr_media_ops;
    media.ctx = &media_data;

    /* a one-block cache: reading the CE area evicts the SUA's sector */
    ASSERT_OK(odfs_cache_init(&cache, &media, 1));

    /* NM "hello_" continued in the CE area */
    s[p + 0] = 'N'; s[p + 1] = 'M'; s[p + 2] = 11; s[p + 3] = 1;
    s[p + 4] = RR_NM_CONTINUE;
    memcpy(&s[p + 5], "hello_", 6);
    p += 11;

    /* CE areas extending past the sector must be ignored */
    p += rr_put_ce(&s[p], ce_rel_lba, 2040, 16);
    p += rr_put_ce(&s[p], ce_rel_lba, 0xFFFFFFF0u, 0x20);

    /* the valid CE, relative to the session */
    p += rr_put_ce(&s[p], ce_rel_lba, 64, 14);

    /* PX after the CE entry must still be parsed from this area */
    s[p + 0] = 'P'; s[p + 1] = 'X'; s[p + 2] = 36; s[p + 3] = 1;
    wr_bb32(&s[p + 4], 0100644);
    p += 36;

    ce[0] = 'N'; ce[1] = 'M'; ce[2] = 14; ce[3] = 1; ce[4] = 0;
    memcpy(&ce[5], "world.txt", 9);

    ASSERT_OK(odfs_cache_read(&cache, 10, &sec));
    rr_parse(sec, p, 0, session_start, &info, NULL, 0, &cache);

    ASSERT_EQ(info.has_name, 1);
    ASSERT_STR_EQ(info.name, "hello_world.txt");
    ASSERT_EQ(info.has_posix, 1);
    ASSERT_EQ(info.mode, 0100644u);

    odfs_cache_destroy(&cache);
}

TEST(rr_detect_sp_after_cd_xa_record)
{
    /* root "." System Use area as written by mkisofs -R -XA */
    const uint8_t xa_sua[] = {
        0, 0, 0, 0, 0x88, 0x80, 'X', 'A', 0, 0, 0, 0, 0, 0,
        'S', 'P', 7, 1, 0xBE, 0xEF, 14
    };
    int skip = -1;

    ASSERT_EQ(rr_detect(xa_sua, sizeof(xa_sua), &skip), 1);
    ASSERT_EQ(skip, 14);
}

static size_t rr_dir_rec(uint8_t *p, uint32_t lba, uint32_t size,
                         uint8_t flags, const char *name, size_t name_len,
                         const uint8_t *sua, size_t sua_len)
{
    size_t name_end = 33 + name_len + ((name_len & 1) == 0 ? 1 : 0);
    size_t rec_len = name_end + sua_len;
    if (rec_len & 1)
        rec_len++;
    memset(p, 0, rec_len);
    p[0] = (uint8_t)rec_len;
    wr_bb32(&p[ISO_DR_EXTENT_LBA], lba);
    wr_bb32(&p[ISO_DR_DATA_LENGTH], size);
    p[ISO_DR_DATE + 0] = 126;
    p[ISO_DR_DATE + 1] = 1;
    p[ISO_DR_DATE + 2] = 1;
    p[ISO_DR_FLAGS] = flags;
    wr_bb16(&p[ISO_DR_VOLUME_SEQ_NUM], 1);
    p[ISO_DR_NAME_LEN] = (uint8_t)name_len;
    memcpy(&p[ISO_DR_NAME], name, name_len);
    if (sua && sua_len > 0)
        memcpy(&p[name_end], sua, sua_len);
    return rec_len;
}

static size_t rr_put_nm(uint8_t *p, const char *name)
{
    size_t n = strlen(name);
    p[0] = 'N'; p[1] = 'M'; p[2] = (uint8_t)(5 + n); p[3] = 1; p[4] = 0;
    memcpy(&p[5], name, n);
    return 5 + n;
}

static size_t rr_put_link(uint8_t *p, char a, char b, uint32_t lba)
{
    memset(p, 0, 12);
    p[0] = (uint8_t)a; p[1] = (uint8_t)b; p[2] = 12; p[3] = 1;
    wr_bb32(&p[4], lba);
    return 12;
}

/*
 * A directory relocated the way mkisofs -R does it: the placeholder in
 * the real parent carries CL, the directory itself lives in rr_moved
 * under an entry marked RE, and its ".." record points at rr_moved with
 * PL naming the real parent. mkisofs marks rr_moved itself RE as well.
 */
TEST(rr_relocated_directory_cl_and_pl)
{
    static rr_media_t media_data;
    odfs_media_t media;
    odfs_mount_t mnt;
    odfs_node_t rr_moved, deep_dir, sub, child_file, parent, grandparent, n;
    char buf[16];
    size_t len, off, k;
    uint8_t sua[96];
    static const uint8_t sp[] = { 'S', 'P', 7, 1, 0xBE, 0xEF, 0 };
    static const uint8_t re[] = { 'R', 'E', 4, 1 };
    const uint32_t root_lba = 20;
    const uint32_t rr_moved_lba = 21;
    const uint32_t relocated_lba = 22;
    const uint32_t file_lba = 23;
    const uint32_t sub_lba = 24;
    uint8_t *pvd = media_data.sectors[ISO_VD_START_LBA];
    uint8_t *term = media_data.sectors[ISO_VD_START_LBA + 1];
    uint8_t *d;

    memset(&media_data, 0, sizeof(media_data));

    pvd[ISO_PVD_TYPE] = ISO_VD_TYPE_PRIMARY;
    memcpy(&pvd[ISO_PVD_ID], ISO_STANDARD_ID, ISO_STANDARD_ID_LEN);
    pvd[ISO_PVD_VERSION] = 1;
    memset(&pvd[ISO_PVD_SYSTEM_ID], ' ', 32);
    memset(&pvd[ISO_PVD_VOLUME_ID], ' ', 32);
    memcpy(&pvd[ISO_PVD_VOLUME_ID], "RRCLPL", 6);
    wr_bb32(&pvd[ISO_PVD_VOLUME_SPACE_SIZE], RR_TEST_SECTORS);
    wr_bb16(&pvd[ISO_PVD_LOGICAL_BLK_SIZE], ISO_SECTOR_SIZE);
    rr_dir_rec(&pvd[ISO_PVD_ROOT_DIR_RECORD], root_lba, ISO_SECTOR_SIZE,
               ISO_DR_FLAG_DIRECTORY, "\x00", 1, NULL, 0);
    term[ISO_PVD_TYPE] = ISO_VD_TYPE_TERM;
    memcpy(&term[ISO_PVD_ID], ISO_STANDARD_ID, ISO_STANDARD_ID_LEN);
    term[ISO_PVD_VERSION] = 1;

    /* root: rr_moved (RE) and the placeholder for deep_dir (CL) */
    d = media_data.sectors[root_lba];
    off = 0;
    off += rr_dir_rec(&d[off], root_lba, ISO_SECTOR_SIZE,
                      ISO_DR_FLAG_DIRECTORY, "\x00", 1, sp, sizeof(sp));
    off += rr_dir_rec(&d[off], root_lba, ISO_SECTOR_SIZE,
                      ISO_DR_FLAG_DIRECTORY, "\x01", 1, NULL, 0);
    k = rr_put_nm(sua, "rr_moved");
    memcpy(&sua[k], re, sizeof(re));
    k += sizeof(re);
    off += rr_dir_rec(&d[off], rr_moved_lba, ISO_SECTOR_SIZE,
                      ISO_DR_FLAG_DIRECTORY, "RR_MOVED", 8, sua, k);
    /* placeholder: a zero-length file whose PX even claims a file */
    memset(sua, 0, sizeof(sua));
    sua[0] = 'P'; sua[1] = 'X'; sua[2] = 36; sua[3] = 1;
    wr_bb32(&sua[4], 0100555);
    k = 36;
    k += rr_put_nm(&sua[k], "deep_dir");
    k += rr_put_link(&sua[k], 'C', 'L', relocated_lba);
    off += rr_dir_rec(&d[off], 0, 0, 0, "DEEP_DIR.;1", 11, sua, k);

    /* rr_moved: the relocated directory's own entry, marked RE */
    d = media_data.sectors[rr_moved_lba];
    off = 0;
    off += rr_dir_rec(&d[off], rr_moved_lba, ISO_SECTOR_SIZE,
                      ISO_DR_FLAG_DIRECTORY, "\x00", 1, NULL, 0);
    off += rr_dir_rec(&d[off], root_lba, ISO_SECTOR_SIZE,
                      ISO_DR_FLAG_DIRECTORY, "\x01", 1, NULL, 0);
    k = rr_put_nm(sua, "deep_dir");
    memcpy(&sua[k], re, sizeof(re));
    k += sizeof(re);
    off += rr_dir_rec(&d[off], relocated_lba, ISO_SECTOR_SIZE,
                      ISO_DR_FLAG_DIRECTORY, "DEEP_DIR", 8, sua, k);

    /* deep_dir: ".." points at rr_moved, PL at the real parent */
    d = media_data.sectors[relocated_lba];
    off = 0;
    off += rr_dir_rec(&d[off], relocated_lba, ISO_SECTOR_SIZE,
                      ISO_DR_FLAG_DIRECTORY, "\x00", 1, NULL, 0);
    k = rr_put_link(sua, 'P', 'L', root_lba);
    off += rr_dir_rec(&d[off], rr_moved_lba, ISO_SECTOR_SIZE,
                      ISO_DR_FLAG_DIRECTORY, "\x01", 1, sua, k);
    k = rr_put_nm(sua, "inside.txt");
    off += rr_dir_rec(&d[off], file_lba, 6, 0, "INSIDE.TXT;1", 12, sua, k);
    k = rr_put_nm(sua, "sub");
    off += rr_dir_rec(&d[off], sub_lba, ISO_SECTOR_SIZE,
                      ISO_DR_FLAG_DIRECTORY, "SUB", 3, sua, k);

    /* deep_dir/sub */
    d = media_data.sectors[sub_lba];
    off = 0;
    off += rr_dir_rec(&d[off], sub_lba, ISO_SECTOR_SIZE,
                      ISO_DR_FLAG_DIRECTORY, "\x00", 1, NULL, 0);
    off += rr_dir_rec(&d[off], relocated_lba, ISO_SECTOR_SIZE,
                      ISO_DR_FLAG_DIRECTORY, "\x01", 1, NULL, 0);

    memcpy(media_data.sectors[file_lba], "Reloc!", 6);

    media.ops = &rr_media_ops;
    media.ctx = &media_data;
    ASSERT_OK(odfs_mount(&media, NULL, NULL, &mnt));

    /* rr_moved and the RE entry inside it stay hidden */
    ASSERT_ERR(odfs_lookup(&mnt, &mnt.root, "rr_moved", &n),
               ODFS_ERR_NOT_FOUND);
    rr_moved = mnt.root;
    rr_moved.extent.lba = rr_moved_lba;
    ASSERT_ERR(odfs_lookup(&mnt, &rr_moved, "deep_dir", &n),
               ODFS_ERR_NOT_FOUND);

    /* the placeholder is the relocated directory, with its real size */
    ASSERT_OK(odfs_lookup(&mnt, &mnt.root, "deep_dir", &deep_dir));
    ASSERT_EQ(deep_dir.kind, ODFS_NODE_DIR);
    ASSERT_EQ(deep_dir.extent.lba, relocated_lba);
    ASSERT_EQ(deep_dir.extent.length, ISO_SECTOR_SIZE);

    ASSERT_OK(odfs_lookup(&mnt, &deep_dir, "inside.txt", &child_file));
    len = sizeof(buf);
    memset(buf, 0, sizeof(buf));
    ASSERT_OK(odfs_read(&mnt, &child_file, 0, buf, &len));
    ASSERT_EQ(len, 6);
    ASSERT(memcmp(buf, "Reloc!", 6) == 0);

    /* walking up follows PL back to the real parent */
    ASSERT_OK(odfs_resolve_parent_node(&mnt, &deep_dir, &parent, NULL));
    ASSERT_EQ(parent.extent.lba, root_lba);

    ASSERT_OK(odfs_lookup(&mnt, &deep_dir, "sub", &sub));
    ASSERT_OK(odfs_resolve_parent_node(&mnt, &sub, &parent, &grandparent));
    ASSERT_EQ(parent.extent.lba, relocated_lba);
    ASSERT_STR_EQ(parent.name, "deep_dir");
    ASSERT_EQ(parent.id, deep_dir.id);
    ASSERT_EQ(grandparent.extent.lba, root_lba);

    odfs_unmount(&mnt);
}

TEST_MAIN()
