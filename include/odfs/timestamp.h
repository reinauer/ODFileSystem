/*
 * odfs/timestamp.h — timestamp conversion helpers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef ODFS_TIMESTAMP_H
#define ODFS_TIMESTAMP_H

#include "odfs/node.h"

#include <stdint.h>

void odfs_timestamp_from_mac_time(uint32_t mac_secs,
                                  odfs_timestamp_t *timestamp);

#endif /* ODFS_TIMESTAMP_H */
