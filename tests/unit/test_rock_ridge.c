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

TEST_MAIN()
