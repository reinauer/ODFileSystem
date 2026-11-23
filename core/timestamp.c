/*
 * timestamp.c — timestamp conversion helpers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "odfs/timestamp.h"

#include <string.h>

#define ODFS_MAC_EPOCH UINT32_C(2082844800)

void odfs_timestamp_from_mac_time(uint32_t mac_secs,
                                  odfs_timestamp_t *timestamp)
{
    uint32_t unix_secs;
    uint32_t days;
    uint32_t rem;
    uint32_t era;
    uint32_t doe;
    uint32_t yoe;
    uint32_t doy;
    uint32_t mp;
    uint32_t year;
    uint32_t month;

    memset(timestamp, 0, sizeof(*timestamp));
    if (mac_secs < ODFS_MAC_EPOCH)
        return;

    unix_secs = mac_secs - ODFS_MAC_EPOCH;
    days = unix_secs / 86400U;
    rem = unix_secs - days * 86400U;
    timestamp->hour = (uint8_t)(rem / 3600U);
    rem -= (uint32_t)timestamp->hour * 3600U;
    timestamp->minute = (uint8_t)(rem / 60U);
    timestamp->second = (uint8_t)(rem - (uint32_t)timestamp->minute * 60U);

    /*
     * Civil date from the day count in closed form instead of stepping
     * through the years (Howard Hinnant's civil_from_days). Days count
     * from 0000-03-01, 719468 days before 1970-01-01, so a leap day
     * ends its year; they split into 400-year eras, years of the era
     * and March-based months.
     */
    days += 719468U;
    era = days / 146097U;
    doe = days - era * 146097U;
    yoe = (doe - doe / 1460U + doe / 36524U - doe / 146096U) / 365U;
    doy = doe - (365U * yoe + yoe / 4U - yoe / 100U);
    mp = (5U * doy + 2U) / 153U;
    month = mp < 10U ? mp + 3U : mp - 9U;
    year = yoe + era * 400U + (month <= 2U);

    timestamp->year = (int32_t)year;
    timestamp->month = (uint8_t)month;
    timestamp->day = (uint8_t)(doy - (153U * mp + 2U) / 5U + 1U);
}
