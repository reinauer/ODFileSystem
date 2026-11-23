/*
 * test_timestamp.c — HFS/HFS+ timestamp conversion
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "odfs/timestamp.h"
#include "test_harness.h"

#define MAC_EPOCH_1970 2082844800U

static int is_leap(uint32_t y)
{
    return y % 4U == 0 && (y % 100U != 0 || y % 400U == 0);
}

/* step through the years and months, as the converter once did */
static void reference_date(uint32_t days, uint32_t *year, uint32_t *month,
                           uint32_t *day)
{
    static const uint8_t mdays[12] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };
    uint32_t y = 1970;
    uint32_t m;

    while (days >= 365U + (uint32_t)is_leap(y)) {
        days -= 365U + (uint32_t)is_leap(y);
        y++;
    }
    for (m = 0; m < 12; m++) {
        uint32_t md = mdays[m] + (m == 1 ? (uint32_t)is_leap(y) : 0U);

        if (days < md)
            break;
        days -= md;
    }
    *year = y;
    *month = m + 1U;
    *day = days + 1U;
}

TEST(mac_time_before_1970_is_zero)
{
    odfs_timestamp_t ts;

    odfs_timestamp_from_mac_time(0, &ts);
    ASSERT_EQ(ts.year, 0);
    ASSERT_EQ(ts.month, 0);
    odfs_timestamp_from_mac_time(MAC_EPOCH_1970 - 1U, &ts);
    ASSERT_EQ(ts.year, 0);
    ASSERT_EQ(ts.day, 0);
}

TEST(mac_time_fixed_dates)
{
    odfs_timestamp_t ts;

    odfs_timestamp_from_mac_time(MAC_EPOCH_1970, &ts);
    ASSERT_EQ(ts.year, 1970);
    ASSERT_EQ(ts.month, 1);
    ASSERT_EQ(ts.day, 1);
    ASSERT_EQ(ts.hour, 0);
    ASSERT_EQ(ts.minute, 0);
    ASSERT_EQ(ts.second, 0);

    /* 2000-02-29 12:34:56, a leap day in a century year */
    odfs_timestamp_from_mac_time(MAC_EPOCH_1970 + 11016U * 86400U + 45296U,
                                 &ts);
    ASSERT_EQ(ts.year, 2000);
    ASSERT_EQ(ts.month, 2);
    ASSERT_EQ(ts.day, 29);
    ASSERT_EQ(ts.hour, 12);
    ASSERT_EQ(ts.minute, 34);
    ASSERT_EQ(ts.second, 56);

    odfs_timestamp_from_mac_time(MAC_EPOCH_1970 + 11017U * 86400U, &ts);
    ASSERT_EQ(ts.year, 2000);
    ASSERT_EQ(ts.month, 3);
    ASSERT_EQ(ts.day, 1);

    /* the last representable second */
    odfs_timestamp_from_mac_time(UINT32_MAX, &ts);
    ASSERT_EQ(ts.year, 2040);
    ASSERT_EQ(ts.month, 2);
    ASSERT_EQ(ts.day, 6);
    ASSERT_EQ(ts.hour, 6);
    ASSERT_EQ(ts.minute, 28);
    ASSERT_EQ(ts.second, 15);
}

TEST(mac_time_matches_year_by_year_count)
{
    const uint32_t last_day = (UINT32_MAX - MAC_EPOCH_1970) / 86400U;
    uint32_t d;

    for (d = 0; d <= last_day; d++) {
        uint32_t secs = (d * 7919U) % 86400U;
        uint32_t year, month, day;
        odfs_timestamp_t ts;

        if (d == last_day)
            secs = (UINT32_MAX - MAC_EPOCH_1970) % 86400U;
        odfs_timestamp_from_mac_time(MAC_EPOCH_1970 + d * 86400U + secs,
                                     &ts);
        reference_date(d, &year, &month, &day);
        ASSERT_EQ(ts.year, (int32_t)year);
        ASSERT_EQ(ts.month, month);
        ASSERT_EQ(ts.day, day);
        ASSERT_EQ(ts.hour, secs / 3600U);
        ASSERT_EQ(ts.minute, secs / 60U % 60U);
        ASSERT_EQ(ts.second, secs % 60U);
    }
}

TEST_MAIN()
