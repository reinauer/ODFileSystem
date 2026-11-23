/*
 * hfsplus.h — HFS+ on-disc structures
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * References: Apple TN1150, Linux fs/hfsplus/hfsplus_raw.h
 */

#ifndef ODFS_HFSPLUS_H
#define ODFS_HFSPLUS_H

#include "odfs/backend.h"
#include "hfs/hfs_common.h"
#include <stdint.h>

/* Volume Header signatures */
#define HFSPLUS_SIG     0x482B  /* 'H+' */
#define HFSX_SIG        0x4858  /* 'HX' (case-sensitive variant) */

/* Volume Header is at offset 1024 from partition start */
#define HFSPLUS_VH_OFFSET  1024

/* volume_name buffer size, NUL included */
#define HFSPLUS_VOLNAME_SIZE 256

extern const odfs_backend_ops_t hfsplus_backend_ops;

#endif /* ODFS_HFSPLUS_H */
