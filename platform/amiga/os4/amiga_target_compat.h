/*
 * amiga_target_compat.h - AmigaOS 4 source compatibility
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef ODFS_AMIGA_TARGET_COMPAT_H
#define ODFS_AMIGA_TARGET_COMPAT_H

/*
 * The current shared packet handler still uses classic names such as
 * DeviceList and ACTION_DIE. Keep that compatibility local to the OS4
 * frontend while the native vector-port frontend is being split out.
 */
#include <dos/obsolete.h>

#endif /* ODFS_AMIGA_TARGET_COMPAT_H */
