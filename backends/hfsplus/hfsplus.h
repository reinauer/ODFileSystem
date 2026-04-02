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
#include <stdint.h>

/* Volume Header signatures */
#define HFSPLUS_SIG     0x482B  /* 'H+' */
#define HFSX_SIG        0x4858  /* 'HX' (case-sensitive variant) */

/* Volume Header is at offset 1024 from partition start */
#define HFSPLUS_VH_OFFSET  1024

/* Special CNIDs */
#define HFSPLUS_CNID_ROOT   2

/* Catalog record types */
#define HFSPLUS_FOLDER_REC   0x0001
#define HFSPLUS_FILE_REC     0x0002
#define HFSPLUS_FOLDER_THREAD 0x0003
#define HFSPLUS_FILE_THREAD  0x0004

/* BE helpers */
static inline uint16_t hfsp_be16(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | p[1];
}

static inline uint32_t hfsp_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | p[3];
}

static inline uint64_t hfsp_be64(const uint8_t *p)
{
    return ((uint64_t)hfsp_be32(p) << 32) | hfsp_be32(p + 4);
}

/* HFS+ fork extent record (8 bytes) */
typedef struct hfsp_extent {
    uint32_t start_block;
    uint32_t block_count;
} hfsp_extent_t;

/* HFS+ fork data (80 bytes on disc) */
typedef struct hfsp_fork {
    uint64_t     logical_size;
    uint32_t     clump_size;
    uint32_t     total_blocks;
    hfsp_extent_t extents[8];
} hfsp_fork_t;

/* HFS+ mount context */
typedef struct hfsplus_context {
    /* partition offset (bytes from image start to HFS+ volume start) */
    uint64_t vol_offset;

    /* from Volume Header */
    uint32_t block_size;
    uint32_t total_blocks;
    char     volume_name[256];  /* decoded from catalog thread of root */

    /* catalog B-Tree */
    hfsp_fork_t cat_fork;
    uint32_t    cat_node_size;
    uint32_t    cat_root_node;
    uint32_t    cat_first_leaf;

    uint32_t next_node_id;
} hfsplus_context_t;

extern const odfs_backend_ops_t hfsplus_backend_ops;

#endif /* ODFS_HFSPLUS_H */
