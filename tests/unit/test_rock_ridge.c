/*
 * test_rock_ridge.c — tests for Rock Ridge parser
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "rock_ridge/rock_ridge.h"
#include "iso9660/iso9660.h"
#include "odfs/cache.h"
#include "test_harness.h"

#include <string.h>

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

TEST_MAIN()
