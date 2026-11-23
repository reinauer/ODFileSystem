/*
 * hfs.c — HFS backend (read-only)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Reads classic Macintosh HFS volumes, commonly found on hybrid
 * ISO/HFS CDs from the 1990s. Data fork only (resource forks are
 * not exposed). The catalog B-tree walk is shared with HFS+ in
 * hfs_common.c.
 *
 * References: Inside Macintosh: Files, Linux fs/hfs/, libhfs
 */

#include "hfs.h"
#include "odfs/alloc.h"
#include "odfs/cache.h"
#include "odfs/charset.h"
#include "odfs/log.h"
#include "odfs/error.h"
#include "odfs/timestamp.h"

#include <string.h>
#include <inttypes.h>

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

/* Mac Roman to UTF-8, then normalize a few Amiga-problematic bytes. */
static void hfs_name_to_utf8(const uint8_t *src, uint8_t src_len,
                              char *dst, size_t dst_size)
{
    size_t out_len = 0;

    odfs_mac_roman_to_utf8(src, src_len, dst, dst_size, &out_len);
    odfs_hfs_fix_name(dst, out_len);
}

/* ------------------------------------------------------------------ */
/* probe: look for MDB at block 2 (with or without partition map)      */
/* ------------------------------------------------------------------ */

static int hfs_check_part(odfs_cache_t *cache, uint64_t part_byte, void *ctx)
{
    const uint8_t *mdb = odfs_hfs_read_block(cache, part_byte + 1024);

    if (!mdb || hfs_be16(mdb) != HFS_MDB_SIG)
        return 0;
    *(uint64_t *)ctx = part_byte;
    return 1;
}

/* find the byte offset of the HFS volume: at the session start or in
 * an Apple_HFS partition */
static int hfs_find_volume(odfs_cache_t *cache, uint32_t session_start,
                           uint64_t *vol_byte)
{
    return hfs_check_part(cache, (uint64_t)session_start * 2048, vol_byte) ||
           odfs_hfs_scan_apm(cache, session_start, hfs_check_part, vol_byte);
}

static odfs_err_t hfs_probe(odfs_cache_t *cache,
                              odfs_log_state_t *log,
                              uint32_t session_start)
{
    uint64_t vol_byte;

    if (!hfs_find_volume(cache, session_start, &vol_byte))
        return ODFS_ERR_BAD_FORMAT;

    ODFS_INFO(log, ODFS_SUB_HFS, "HFS MDB found at byte %" PRIu64,
              vol_byte + 1024);
    return ODFS_OK;
}

/* ------------------------------------------------------------------ */
/* catalog records                                                     */
/* ------------------------------------------------------------------ */

static int hfs_parse_entry(const hfs_volume_t *vol, const odfs_node_t *dir,
                           const uint8_t *key, size_t key_len,
                           const uint8_t *data, size_t data_len,
                           odfs_node_t *node)
{
    uint8_t rec_type;

    (void)vol;

    if (data_len < 1)
        return 0;

    rec_type = data[0];

    /* only interested in file and directory records, not threads */
    if (rec_type != HFS_CAT_FILE && rec_type != HFS_CAT_DIR)
        return 0;

    /* hide what the Finder hides: fdFlags of a file's FInfo, frFlags
     * of a directory's DInfo */
    if (rec_type == HFS_CAT_FILE && data_len >= 14 &&
        (hfs_be16(&data[12]) & HFS_FINDER_INVISIBLE))
        return 0;
    if (rec_type == HFS_CAT_DIR && data_len >= 32 &&
        (hfs_be16(&data[30]) & HFS_FINDER_INVISIBLE))
        return 0;

    memset(node, 0, sizeof(*node));
    node->parent_id = dir->id;
    node->backend = ODFS_BACKEND_HFS;

    /* extract name from catalog key:
       key[0] = key length, key[1] = reserved, key[2..5] = parentID,
       key[6] = name length, key[7..] = name bytes */
    if (key_len >= 8) {
        uint8_t nlen = key[6];
        if (nlen > 0 && (size_t)(7 + nlen) <= key_len)
            hfs_name_to_utf8(&key[7], nlen, node->name, sizeof(node->name));
    }

    if (rec_type == HFS_CAT_DIR && data_len >= 22) {
        node->kind = ODFS_NODE_DIR;
        node->id = hfs_be32(&data[6]);         /* dirDirID */
        node->extent.lba = node->id;           /* store CNID for readdir */
        odfs_timestamp_from_mac_time(hfs_be32(&data[14]), &node->mtime);
    } else if (rec_type == HFS_CAT_FILE && data_len >= 52) {
        node->kind = ODFS_NODE_FILE;
        node->id = hfs_be32(&data[20]);   /* filFlNum */
        node->size = hfs_be32(&data[26]); /* data fork logical length */
        /* data fork first extent: at offset 74 in file record */
        if (data_len >= 78) {
            node->extent.lba = hfs_be16(&data[74]); /* first extent start AB */
            node->extent.length = (uint32_t)node->size;
        }
        odfs_timestamp_from_mac_time(hfs_be32(&data[48]), &node->mtime);
    }
    node->ctime = node->mtime;
    return 1;
}

/* ------------------------------------------------------------------ */
/* mount                                                               */
/* ------------------------------------------------------------------ */

static odfs_err_t hfs_mount(odfs_cache_t *cache,
                              odfs_log_state_t *log,
                              uint32_t session_start,
                              odfs_node_t *root_out,
                              void **backend_ctx)
{
    hfs_volume_t *vol;
    const uint8_t *mdb;
    odfs_err_t err;
    uint64_t vol_byte;
    uint32_t mtime;

    vol = odfs_calloc(1, sizeof(*vol) + HFS_VOLNAME_SIZE);
    if (!vol) return ODFS_ERR_NOMEM;

    vol->parse_entry = hfs_parse_entry;

    if (!hfs_find_volume(cache, session_start, &vol_byte)) {
        odfs_free(vol);
        return ODFS_ERR_BAD_FORMAT;
    }
    mdb = odfs_hfs_read_block(cache, vol_byte + 1024);
    if (!mdb) {
        odfs_free(vol);
        return ODFS_ERR_IO;
    }

    /* parse MDB */
    vol->total_blocks = hfs_be16(&mdb[HFS_MDB_NMALBLKS]);
    vol->block_size = hfs_be32(&mdb[HFS_MDB_ALBLKSIZ]);
    vol->alloc_base = vol_byte + (uint64_t)hfs_be16(&mdb[HFS_MDB_ALBLST]) * 512;

    uint8_t vnamelen = mdb[HFS_MDB_VOLNAMELEN];
    if (vnamelen > 27) vnamelen = 27;
    hfs_name_to_utf8(&mdb[HFS_MDB_VOLNAME], vnamelen,
                     vol->volume_name, HFS_VOLNAME_SIZE);

    for (int i = 0; i < 3; i++) {
        vol->cat.extents[i].start_block = hfs_be16(&mdb[HFS_MDB_CTEXTREC + i * 4]);
        vol->cat.extents[i].block_count = hfs_be16(&mdb[HFS_MDB_CTEXTREC + i * 4 + 2]);
    }

    /* MDB modification date for root timestamp */
    mtime = hfs_be32(&mdb[HFS_MDB_LSMOD]);

    ODFS_INFO(log, ODFS_SUB_HFS,
               "volume: \"%s\", alloc blocks: %" PRIu32 ", block size: %" PRIu32,
               vol->volume_name, vol->total_blocks, vol->block_size);

    /* read catalog B-tree header node */
    err = odfs_hfs_init_btree(vol, &vol->cat, cache);
    if (err != ODFS_OK) {
        if (err == ODFS_ERR_BAD_FORMAT)
            ODFS_ERROR(log, ODFS_SUB_HFS, "bad catalog B-tree header");
        odfs_free(vol);
        return err;
    }

    err = odfs_hfs_alloc_node_buf(vol);
    if (err != ODFS_OK) {
        odfs_free(vol);
        return err;
    }

    ODFS_INFO(log, ODFS_SUB_HFS,
               "catalog: root node %" PRIu32 ", node size %" PRIu32,
               vol->cat.root_node, vol->cat.node_size);

    /* build root node */
    memset(root_out, 0, sizeof(*root_out));
    root_out->id = 0;
    root_out->parent_id = 0;
    root_out->backend = ODFS_BACKEND_HFS;
    root_out->kind = ODFS_NODE_DIR;
    root_out->name[0] = '/';
    root_out->name[1] = '\0';
    /* store root CNID in extent.lba for use by readdir */
    root_out->extent.lba = HFS_CNID_ROOT_DIR;
    root_out->extent.length = 0;

    odfs_timestamp_from_mac_time(mtime, &root_out->mtime);
    root_out->ctime = root_out->mtime;
    vol->root = *root_out;

    *backend_ctx = vol;
    return ODFS_OK;
}

/* ------------------------------------------------------------------ */
/* backend ops table                                                   */
/* ------------------------------------------------------------------ */

const odfs_backend_ops_t hfs_backend_ops = {
    .name            = "hfs",
    .backend_type    = ODFS_BACKEND_HFS,
    .probe           = hfs_probe,
    .mount           = hfs_mount,
    .unmount         = odfs_hfs_unmount,
    .readdir         = odfs_hfs_readdir,
    .read            = odfs_hfs_read,
    .lookup          = odfs_hfs_lookup,
    .resolve_parent  = odfs_hfs_resolve_parent,
    .get_volume_name = odfs_hfs_get_volume_name,
    .get_volume_size = odfs_hfs_get_volume_size,
};
