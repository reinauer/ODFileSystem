/*
 * test_rock_ridge.c — tests for Rock Ridge parser
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "rock_ridge/rock_ridge.h"
#include "test_harness.h"

TEST(rr_parse_sl_basic)
{
    const uint8_t sua[] = {
        'S', 'L', 16, 1, 0,
        0, 3, 'e', 't', 'c',
        0, 4, 'h', 'o', 's', 't',
        'S', 'T', 4, 1
    };
    rr_info_t info;

    rr_parse(sua, sizeof(sua), 0, &info, NULL);

    ASSERT_EQ(info.is_symlink, 1);
    ASSERT_STR_EQ(info.symlink_target, "etc/host");
}

TEST(rr_parse_sl_truncated_component)
{
    const uint8_t sua[] = {
        'S', 'L', 8, 1, 0,
        0, 10, 'x',
        'S', 'T', 4, 1
    };
    rr_info_t info;

    rr_parse(sua, sizeof(sua), 0, &info, NULL);

    ASSERT_EQ(info.is_symlink, 1);
    ASSERT_STR_EQ(info.symlink_target, "");
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

    rr_parse(sua, sizeof(sua), 0, &info, NULL);

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

    rr_parse(sua, sizeof(sua), 0, &info, NULL);

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

    rr_parse(short_tf, 4, 0, &info, NULL);
    ASSERT_EQ(info.has_mtime, 0);
    ASSERT_EQ(info.has_ctime, 0);

    rr_parse(tf7, sizeof(tf7), 0, &info, NULL);
    ASSERT_EQ(info.has_mtime, 1);
    ASSERT_EQ(info.has_ctime, 0);
    ASSERT_EQ(info.mtime.year, 2026);
    ASSERT_EQ(info.mtime.month, 5);
    ASSERT_EQ(info.mtime.day, 19);
    ASSERT_EQ(info.mtime.tz_offset, 60);

    rr_parse(tf17, sizeof(tf17), 0, &info, NULL);
    ASSERT_EQ(info.has_mtime, 0);
    ASSERT_EQ(info.has_ctime, 1);
    ASSERT_EQ(info.ctime.year, 2026);
    ASSERT_EQ(info.ctime.month, 11);
    ASSERT_EQ(info.ctime.day, 21);
    ASSERT_EQ(info.ctime.second, 33);
    ASSERT_EQ(info.ctime.tz_offset, -60);
}

TEST_MAIN()
