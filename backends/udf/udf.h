/*
 * udf.h — UDF on-disc structures and backend interface
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * References: ECMA-167, OSTA UDF 2.01/2.50/2.60
 */

#ifndef ODFS_UDF_H
#define ODFS_UDF_H

#include "odfs/backend.h"
#include <stdint.h>

/* ---- tag identifiers (ECMA-167 Table 3/7) ---- */

#define UDF_TAG_PVD       1    /* Primary Volume Descriptor */
#define UDF_TAG_AVDP      2    /* Anchor Volume Descriptor Pointer */
#define UDF_TAG_VDP       3    /* Volume Descriptor Pointer */
#define UDF_TAG_IUVD      4    /* Implementation Use Volume Descriptor */
#define UDF_TAG_PD        5    /* Partition Descriptor */
#define UDF_TAG_LVD       6    /* Logical Volume Descriptor */
#define UDF_TAG_USD       7    /* Unallocated Space Descriptor */
#define UDF_TAG_TD        8    /* Terminating Descriptor */
#define UDF_TAG_LVID      9    /* Logical Volume Integrity Descriptor */
#define UDF_TAG_FSD      256   /* File Set Descriptor */
#define UDF_TAG_FID      257   /* File Identifier Descriptor */
#define UDF_TAG_AED      258   /* Allocation Extent Descriptor */
#define UDF_TAG_IE       259   /* Indirect Entry */
#define UDF_TAG_TE       260   /* Terminal Entry */
#define UDF_TAG_FE       261   /* File Entry */
#define UDF_TAG_EAHD     262   /* Extended Attribute Header Descriptor */
#define UDF_TAG_EFE      266   /* Extended File Entry */

/* ---- AVDP location ---- */

#define UDF_AVDP_LBA     256

/* ---- ICB file types (ECMA-167 14.6.6) ---- */

#define UDF_ICB_FILETYPE_DIR        4
#define UDF_ICB_FILETYPE_FILE       5
#define UDF_ICB_FILETYPE_SYMLINK   12

/* ---- FID flags ---- */

#define UDF_FID_FLAG_HIDDEN    0x01
#define UDF_FID_FLAG_DIRECTORY 0x02
#define UDF_FID_FLAG_DELETED   0x04
#define UDF_FID_FLAG_PARENT    0x08

/* ---- allocation descriptor types (from ICB tag) ---- */

#define UDF_ICB_ALLOC_SHORT     0
#define UDF_ICB_ALLOC_LONG      1
#define UDF_ICB_ALLOC_EXTENDED  2
#define UDF_ICB_ALLOC_EMBEDDED  3

/* ---- helpers for reading UDF LE fields ---- */

static inline uint16_t udf_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t udf_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint64_t udf_le64(const uint8_t *p)
{
    return (uint64_t)udf_le32(p) | ((uint64_t)udf_le32(p + 4) << 32);
}

/* ---- descriptor tag (ECMA-167 7.2.1) ---- */

typedef struct udf_tag {
    uint16_t id;
    uint16_t version;
    uint8_t  checksum;
    uint16_t serial;
    uint16_t crc;
    uint16_t crc_length;
    uint32_t location;
} udf_tag_t;

/* ---- extent (ECMA-167 7.1) ---- */

typedef struct udf_extent {
    uint32_t length;    /* in bytes */
    uint32_t location;  /* LBA */
} udf_extent_t;

/* ---- long allocation descriptor (ECMA-167 14.14.2.2) ---- */

typedef struct udf_long_ad {
    uint32_t length;
    uint32_t lba;        /* logical block within partition */
    uint16_t partition;
} udf_long_ad_t;

/* ---- short allocation descriptor (ECMA-167 14.14.2.1) ---- */

typedef struct udf_short_ad {
    uint32_t length;     /* 30 bits length + 2 bits type */
    uint32_t position;   /* logical block within partition */
} udf_short_ad_t;

/* ---- parsed UDF mount context ---- */

typedef struct udf_context {
    /* partition info */
    uint32_t part_start;     /* partition start LBA (physical) */
    uint32_t part_length;    /* partition length in sectors */
    uint16_t part_number;    /* partition reference number */

    /* logical volume info */
    uint32_t lv_block_size;  /* logical block size */
    char     volume_id[128]; /* volume identifier (UTF-8) */

    /* root directory */
    uint32_t root_icb_lba;   /* root directory ICB LBA (partition-relative) */
    uint16_t root_icb_part;

    /* node tracking */
    uint32_t next_node_id;
} udf_context_t;

/* ---- backend ops ---- */

extern const odfs_backend_ops_t udf_backend_ops;

#endif /* ODFS_UDF_H */
