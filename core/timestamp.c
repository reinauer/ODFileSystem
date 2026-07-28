/*
 * timestamp.c — timestamp conversion helpers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "odfs/timestamp.h"

#include <string.h>

#define ODFS_MAC_EPOCH 2082844800UL

void odfs_timestamp_from_mac_time(uint32_t mac_secs,
                                  odfs_timestamp_t *timestamp)
{
    static const int mdays[] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };
    uint32_t unix_secs;
    uint32_t days;
    uint32_t rem;
    int leap;
    int year;
    int month;

    memset(timestamp, 0, sizeof(*timestamp));
    if (mac_secs < ODFS_MAC_EPOCH)
        return;

    unix_secs = mac_secs - ODFS_MAC_EPOCH;
    days = unix_secs / 86400;
    rem = unix_secs % 86400;
    timestamp->hour = rem / 3600;
    timestamp->minute = (rem % 3600) / 60;
    timestamp->second = rem % 60;

    year = 1970;
    while (1) {
        int year_days =
            365 + ((year % 4 == 0 &&
                    (year % 100 != 0 || year % 400 == 0)) ? 1 : 0);

        if (days < (uint32_t)year_days)
            break;
        days -= year_days;
        year++;
    }
    timestamp->year = year;

    leap = (year % 4 == 0 &&
            (year % 100 != 0 || year % 400 == 0)) ? 1 : 0;
    for (month = 0; month < 12; month++) {
        int month_days = mdays[month] + (month == 1 ? leap : 0);

        if (days < (uint32_t)month_days)
            break;
        days -= month_days;
    }
    timestamp->month = month + 1;
    timestamp->day = days + 1;
}
