/*
 * hfs.h — HFS (Hierarchical File System) on-disc structures
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * References: Inside Macintosh: Files, Linux fs/hfs/hfs.h
 */

#ifndef ODFS_HFS_H
#define ODFS_HFS_H

#include "odfs/backend.h"
#include <stdint.h>

/* ---- MDB (Master Directory Block) ---- */

#define HFS_MDB_SIG          0x4244   /* "BD" big-endian */
#define HFS_MDB_OFFSET_512   2        /* 512-byte block 2 from partition start */

/* MDB field offsets (all big-endian) */
#define HFS_MDB_SIGWORD       0    /* uint16 */
#define HFS_MDB_CRDATE        2    /* uint32 */
#define HFS_MDB_LSMOD         6    /* uint32 */
#define HFS_MDB_NMFLS        12    /* uint16: files in root */
#define HFS_MDB_NMALBLKS     18    /* uint16: allocation block count */
#define HFS_MDB_ALBLKSIZ     20    /* uint32: allocation block size */
#define HFS_MDB_ALBLST       28    /* uint16: first alloc block (512-blk offset) */
#define HFS_MDB_FREEBKS      34    /* uint16: free blocks */
#define HFS_MDB_VOLNAMELEN   36    /* uint8: volume name length */
#define HFS_MDB_VOLNAME      37    /* 27 bytes: volume name (Mac Roman) */
#define HFS_MDB_NMRTDIRS     82    /* uint16: dirs in root */
#define HFS_MDB_FILCNT       84    /* uint32: file count */
#define HFS_MDB_DIRCNT       88    /* uint32: directory count */
#define HFS_MDB_XTFLSIZE    130    /* uint32: extents B-tree file size */
#define HFS_MDB_XTEXTREC    134    /* 12 bytes: extents B-tree extents */
#define HFS_MDB_CTFLSIZE    146    /* uint32: catalog B-tree file size */
#define HFS_MDB_CTEXTREC    150    /* 12 bytes: catalog B-tree extents */

/* ---- Apple Partition Map ---- */

#define HFS_APM_SIG           0x504D  /* "PM" */

/* ---- B-Tree node ---- */

#define HFS_NODE_INDEX    0x00
#define HFS_NODE_HEADER   0x01
#define HFS_NODE_MAP      0x02
#define HFS_NODE_LEAF     0xFF

/* ---- catalog record types ---- */

#define HFS_CAT_DIR       1
#define HFS_CAT_FILE      2
#define HFS_CAT_DTHREAD   3
#define HFS_CAT_FTHREAD   4

/* ---- special CNIDs ---- */

#define HFS_CNID_ROOT_PAR    1  /* parent of root directory */
#define HFS_CNID_ROOT_DIR    2  /* root directory ID */

/* ---- big-endian helpers ---- */

static inline uint16_t hfs_be16(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | p[1];
}

static inline uint32_t hfs_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | p[3];
}

/* ---- HFS extent descriptor (4 bytes) ---- */

typedef struct hfs_extent {
    uint16_t start_ab;   /* starting allocation block */
    uint16_t num_ab;     /* number of allocation blocks */
} hfs_extent_t;

/* ---- HFS mount context ---- */

typedef struct hfs_context {
    /* from partition map / direct detection */
    uint32_t vol_start_512;    /* HFS volume start in 512-byte blocks */

    /* from MDB */
    uint16_t num_alloc_blocks;
    uint32_t alloc_block_size;
    uint16_t alloc_block_start; /* in 512-byte blocks from vol start */
    char     volume_name[28];

    /* catalog B-tree */
    uint32_t cat_file_size;
    hfs_extent_t cat_extents[3];
    uint32_t cat_node_size;
    uint32_t cat_root_node;
    uint32_t cat_first_leaf;

    /* node tracking */
    uint32_t next_node_id;
} hfs_context_t;

extern const odfs_backend_ops_t hfs_backend_ops;

#endif /* ODFS_HFS_H */
