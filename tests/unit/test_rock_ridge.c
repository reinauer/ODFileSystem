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

TEST_MAIN()
