/*
 * hfs_common.h — code shared by the HFS and HFS+ backends
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * References: Inside Macintosh: Files, Apple TN1150
 */

#ifndef ODFS_HFS_COMMON_H
#define ODFS_HFS_COMMON_H

#include "odfs/backend.h"
#include <stddef.h>
#include <stdint.h>

/* ---- B-tree node kinds ---- */

#define HFS_NODE_INDEX    0x00
#define HFS_NODE_HEADER   0x01
#define HFS_NODE_LEAF     0xFF

/* ---- catalog record types (one byte on HFS, two on HFS+) ---- */

#define HFS_CAT_DIR       1
#define HFS_CAT_FILE      2
#define HFS_CAT_DTHREAD   3

/* ---- Apple Partition Map ---- */

#define HFS_APM_SIG       0x504D  /* "PM" */

/* ---- special CNIDs ---- */

#define HFS_CNID_ROOT_DIR 2   /* root directory ID */

/* ---- big-endian helpers ---- */

static inline uint16_t hfs_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static inline uint32_t hfs_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | p[3];
}

static inline uint64_t hfs_be64(const uint8_t *p)
{
    return ((uint64_t)hfs_be32(p) << 32) | hfs_be32(p + 4);
}

/* ---- extents and B-trees ---- */

typedef struct hfs_extent {
    uint32_t start_block;   /* first allocation block */
    uint32_t block_count;   /* number of allocation blocks */
} hfs_extent_t;

/* a B-tree file; HFS forks use the first three extents, HFS+ all eight */
typedef struct hfs_btree {
    hfs_extent_t extents[8];
    uint32_t     root_node;
    uint32_t     node_size;
    uint32_t     total_nodes;
} hfs_btree_t;

typedef struct hfs_volume hfs_volume_t;

/*
 * Turn one catalog leaf record of directory dir into *out.
 * Returns 1 for a file or directory entry, 0 for anything to skip.
 */
typedef int (*hfs_parse_entry_fn)(const hfs_volume_t *vol,
                                  const odfs_node_t *dir,
                                  const uint8_t *key, size_t key_len,
                                  const uint8_t *data, size_t data_len,
                                  odfs_node_t *out);

/* mount context shared by both backends */
struct hfs_volume {
    uint64_t           alloc_base;    /* byte offset of allocation block 0 */
    uint32_t           block_size;    /* allocation block size in bytes */
    uint32_t           total_blocks;  /* allocation block count */
    uint8_t            hfsplus;       /* catalog uses HFS+ record layout */
    hfs_parse_entry_fn parse_entry;
    hfs_btree_t        cat;           /* catalog B-tree */
    uint8_t           *node_buf;      /* one B-tree node, for the walk */
    odfs_node_t        root;
    char               volume_name[]; /* sized by the backend */
};

/*
 * Check whether a partition starting at byte part_byte holds the
 * backend's volume; nonzero stops the partition map scan.
 */
typedef int (*hfs_part_fn)(odfs_cache_t *cache, uint64_t part_byte,
                           void *ctx);

typedef odfs_err_t (*hfs_record_fn)(const uint8_t *key, size_t key_len,
                                    const uint8_t *data, size_t data_len,
                                    void *cb_ctx);

const uint8_t *odfs_hfs_read_block(odfs_cache_t *cache, uint64_t byte_off);
int odfs_hfs_scan_apm(odfs_cache_t *cache, uint32_t session_start,
                      hfs_part_fn check, void *ctx);

odfs_err_t odfs_hfs_init_btree(const hfs_volume_t *vol, hfs_btree_t *tree,
                               odfs_cache_t *cache);
odfs_err_t odfs_hfs_alloc_node_buf(hfs_volume_t *vol);

odfs_err_t odfs_hfs_walk_catalog(const hfs_volume_t *vol,
                                 odfs_cache_t *cache, uint32_t parent_cnid,
                                 hfs_record_fn callback, void *cb_ctx);

/* backend ops shared by HFS and HFS+ */
void odfs_hfs_unmount(void *backend_ctx);
odfs_err_t odfs_hfs_readdir(void *backend_ctx, odfs_cache_t *cache,
                            odfs_log_state_t *log, const odfs_node_t *dir,
                            odfs_dir_iter_fn callback, void *cb_ctx,
                            uint32_t *resume_offset);
odfs_err_t odfs_hfs_read(void *backend_ctx, odfs_cache_t *cache,
                         odfs_log_state_t *log, const odfs_node_t *file,
                         uint64_t offset, void *buf, size_t *len);
odfs_err_t odfs_hfs_lookup(void *backend_ctx, odfs_cache_t *cache,
                           odfs_log_state_t *log, const odfs_node_t *dir,
                           const char *name, odfs_node_t *out);
odfs_err_t odfs_hfs_resolve_parent(void *backend_ctx, odfs_cache_t *cache,
                                   odfs_log_state_t *log,
                                   const odfs_node_t *dir,
                                   odfs_node_t *parent_out,
                                   odfs_node_t *grandparent_out);
odfs_err_t odfs_hfs_get_volume_name(void *backend_ctx,
                                    char *buf, size_t buf_size);
uint32_t odfs_hfs_get_volume_size(void *backend_ctx);

#endif /* ODFS_HFS_COMMON_H */
