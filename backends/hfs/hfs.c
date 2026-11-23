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

    for (size_t i = 0; i < out_len && dst[i] != '\0'; i++) {
        unsigned char c = (unsigned char)dst[i];

        if (c == ':')
            dst[i] = '.';
        else if (c == '/')
            dst[i] = '-';
        else if (c < 0x20 || c == 0x7F)
            dst[i] = '?';
    }
}

/* ------------------------------------------------------------------ */
/* probe: look for MDB at block 2 (with or without partition map)      */
/* ------------------------------------------------------------------ */

static odfs_err_t hfs_probe(odfs_cache_t *cache,
                              odfs_log_state_t *log,
                              uint32_t session_start)
{
    const uint8_t *sector;
    odfs_err_t err;

    /* MDB is at 512-byte block 2 from volume start.
       On a 2048-byte CD sector, that's at byte 1024 within LBA 0. */
    err = odfs_cache_read(cache, session_start, &sector);
    if (err != ODFS_OK)
        return err;

    uint16_t sig = hfs_be16(&sector[1024]);
    if (sig == HFS_MDB_SIG) {
        ODFS_INFO(log, ODFS_SUB_HFS, "HFS MDB found at session LBA %" PRIu32,
                   session_start);
        return ODFS_OK;
    }

    /* try with Apple Partition Map: check for "PM" at 512-byte block 1 */
    sig = hfs_be16(&sector[512]);
    if (sig == HFS_APM_SIG) {
        /* scan partition map for Apple_HFS partition */
        /* for now, just check the first partition entry */
        const uint8_t *pm = &sector[512];
        const char *ptype = (const char *)&pm[48]; /* pmPartType at offset 48 */
        if (memcmp(ptype, "Apple_HFS", 9) == 0) {
            uint32_t pstart = hfs_be32(&pm[8]); /* pmPyPartStart */
            /* verify MDB at pstart + 2 */
            uint64_t mdb_byte = ((uint64_t)pstart + 2) * 512;
            uint32_t mdb_lba = (uint32_t)(mdb_byte / 2048);
            uint32_t mdb_off = (uint32_t)(mdb_byte % 2048);
            err = odfs_cache_read(cache, session_start + mdb_lba, &sector);
            if (err != ODFS_OK)
                return err;
            if (hfs_be16(&sector[mdb_off]) == HFS_MDB_SIG) {
                ODFS_INFO(log, ODFS_SUB_HFS,
                           "HFS via APM at partition start %" PRIu32, pstart);
                return ODFS_OK;
            }
        }
    }

    return ODFS_ERR_BAD_FORMAT;
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
    const uint8_t *sector;
    const uint8_t *mdb;
    odfs_err_t err;
    uint32_t vol_start_512 = 0;
    uint32_t mtime;

    vol = odfs_calloc(1, sizeof(*vol) + HFS_VOLNAME_SIZE);
    if (!vol) return ODFS_ERR_NOMEM;

    vol->parse_entry = hfs_parse_entry;

    /* find MDB — try direct first, then APM */
    err = odfs_cache_read(cache, session_start, &sector);
    if (err != ODFS_OK) { odfs_free(vol); return err; }

    if (hfs_be16(&sector[1024]) == HFS_MDB_SIG) {
        vol_start_512 = session_start * 4; /* 2048/512 = 4 */
        mdb = &sector[1024];
    } else if (hfs_be16(&sector[512]) == HFS_APM_SIG) {
        const uint8_t *pm = &sector[512];
        if (memcmp(&pm[48], "Apple_HFS", 9) != 0) {
            odfs_free(vol); return ODFS_ERR_BAD_FORMAT;
        }
        uint32_t pstart = hfs_be32(&pm[8]);
        vol_start_512 = session_start * 4 + pstart;
        uint64_t mdb_byte = ((uint64_t)vol_start_512 + 2) * 512;
        uint32_t mdb_lba = (uint32_t)(mdb_byte / 2048);
        uint32_t mdb_off = (uint32_t)(mdb_byte % 2048);
        err = odfs_cache_read(cache, mdb_lba, &sector);
        if (err != ODFS_OK) { odfs_free(vol); return err; }
        mdb = &sector[mdb_off];
        if (hfs_be16(mdb) != HFS_MDB_SIG) { odfs_free(vol); return ODFS_ERR_BAD_FORMAT; }
    } else {
        odfs_free(vol); return ODFS_ERR_BAD_FORMAT;
    }

    /* parse MDB */
    vol->total_blocks = hfs_be16(&mdb[HFS_MDB_NMALBLKS]);
    vol->block_size = hfs_be32(&mdb[HFS_MDB_ALBLKSIZ]);
    vol->alloc_base = ((uint64_t)vol_start_512 +
                       hfs_be16(&mdb[HFS_MDB_ALBLST])) * 512;

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
