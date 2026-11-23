/*
 * hfsplus.c — HFS+ backend (read-only)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Reads HFS+ volumes. Data fork only. Handles volumes with or
 * without GPT/APM partition wrappers by scanning for the 'H+'
 * signature. B-Tree nodes are typically 4096 bytes. The catalog
 * B-tree walk is shared with HFS in backends/hfs/hfs_common.c.
 *
 * References: Apple TN1150, Linux fs/hfsplus/
 */

#include "hfsplus.h"
#include "hfs/hfs.h"
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

/* decode HFS+ Unicode name (big-endian UTF-16) to UTF-8 */
static void hfsp_decode_name(const uint8_t *uname, uint16_t ulen,
                              char *dst, size_t dst_size)
{
    /* uname is big-endian UTF-16 chars, ulen is number of chars */
    size_t byte_len = (size_t)ulen * 2;
    size_t out_len;
    odfs_ucs2be_to_utf8(uname, byte_len, dst, dst_size, &out_len);
    /* replace ':' with '.' (Mac path separator) */
    for (size_t i = 0; i < out_len; i++)
        if (dst[i] == ':') dst[i] = '.';
}

/* ------------------------------------------------------------------ */
/* probe — scan for H+ signature                                       */
/* ------------------------------------------------------------------ */

static int hfsp_check_part(odfs_cache_t *cache, uint64_t part_byte, void *ctx)
{
    const uint8_t *vh = odfs_hfs_read_block(cache, part_byte + HFSPLUS_VH_OFFSET);
    uint16_t sig;

    if (!vh)
        return 0;
    sig = hfs_be16(vh);

    /*
     * An HFS wrapper: an HFS volume whose MDB points at an HFS+ volume
     * embedded in its allocation blocks (drEmbedExtent).
     */
    if (sig == HFS_MDB_SIG && hfs_be16(&vh[HFS_MDB_EMBEDSIG]) == HFSPLUS_SIG) {
        part_byte += (uint64_t)hfs_be16(&vh[HFS_MDB_ALBLST]) * 512 +
                     (uint64_t)hfs_be16(&vh[HFS_MDB_EMBEDEXT]) *
                     hfs_be32(&vh[HFS_MDB_ALBLKSIZ]);
        vh = odfs_hfs_read_block(cache, part_byte + HFSPLUS_VH_OFFSET);
        if (!vh)
            return 0;
        sig = hfs_be16(vh);
    }

    if (sig != HFSPLUS_SIG && sig != HFSX_SIG)
        return 0;
    *(uint64_t *)ctx = part_byte + HFSPLUS_VH_OFFSET;
    return 1;
}

static odfs_err_t hfsp_find_vh(odfs_cache_t *cache,
                                 uint32_t session_start,
                                 uint64_t *vh_offset_out)
{
    /*
     * Try common locations for the Volume Header:
     * 1. Byte 1024 from image start (bare or HFS-wrapped HFS+ volume)
     * 2. An Apple_HFS or Apple_HFSX partition in an Apple Partition Map
     * 3. Scan 512-byte boundaries in first 256K for 'H+' or 'HX'
     *    (handles GPT and other partitioned images)
     */
    const uint8_t *sector;

    if (hfsp_check_part(cache, (uint64_t)session_start * 2048, vh_offset_out) ||
        odfs_hfs_scan_apm(cache, session_start, hfsp_check_part, vh_offset_out))
        return ODFS_OK;

    /* scan first 256K for the signature */
    for (uint32_t lba = session_start; lba < session_start + 128; lba++) {
        if (odfs_cache_read(cache, lba, &sector) != ODFS_OK)
            continue;
        for (uint32_t off = 0; off <= 2048U - 2U; off += 512U) {
            uint16_t sig = hfs_be16(&sector[off]);
            if (sig == HFSPLUS_SIG || sig == HFSX_SIG) {
                /* VH is at this offset; volume start is 1024 before it */
                uint64_t vh_byte = (uint64_t)lba * 2048U + off;
                *vh_offset_out = vh_byte;
                return ODFS_OK;
            }
        }
    }

    return ODFS_ERR_BAD_FORMAT;
}

static odfs_err_t hfsp_probe(odfs_cache_t *cache,
                               odfs_log_state_t *log,
                               uint32_t session_start)
{
    uint64_t vh_offset;
    odfs_err_t err = hfsp_find_vh(cache, session_start, &vh_offset);
    if (err == ODFS_OK)
        ODFS_INFO(log, ODFS_SUB_HFSPLUS,
                   "HFS+ Volume Header at byte offset %" PRIu64, vh_offset);
    return err;
}

/* ------------------------------------------------------------------ */
/* catalog records                                                     */
/* ------------------------------------------------------------------ */

static int hfsp_parse_entry(const hfs_volume_t *vol, const odfs_node_t *dir,
                            const uint8_t *key, size_t key_len,
                            const uint8_t *data, size_t data_len,
                            odfs_node_t *fnode)
{
    uint16_t key_namelen;
    uint16_t rec_type;

    (void)vol;

    if (key_len < 8 || data_len < 2)
        return 0;

    rec_type = hfs_be16(data);
    if (rec_type == HFS_CAT_DIR) {
        if (data_len < 24)
            return 0;
    } else if (rec_type == HFS_CAT_FILE) {
        if (data_len < 88)
            return 0;
    } else {
        return 0;
    }

    key_namelen = hfs_be16(&key[6]);
    if (key_namelen == 0)
        return 0;
    if (key_namelen >= 4 && key_len >= 10 &&
        key[8] == 0x00 && key[9] == 0x2E)
        return 0;

    memset(fnode, 0, sizeof(*fnode));
    fnode->parent_id = dir->id;
    fnode->backend = ODFS_BACKEND_HFSPLUS;

    if (key_namelen <= (key_len - 8) / 2)
        hfsp_decode_name(&key[8], key_namelen,
                         fnode->name, sizeof(fnode->name));

    if (fnode->name[0] == '\0')
        return 0;

    /* folderID or fileID */
    fnode->id = hfs_be32(&data[8]);
    if (rec_type == HFS_CAT_DIR) {
        fnode->kind = ODFS_NODE_DIR;
        fnode->extent.lba = fnode->id;
    } else {
        fnode->kind = ODFS_NODE_FILE;
        if (data_len >= 168) {
            /* data fork: logicalSize(8) clumpSize(4) totalBlocks(4)
               extents[8] at record offset 88 */
            fnode->size = hfs_be64(&data[88]);
            fnode->extent.lba = hfs_be32(&data[104]);
            fnode->extent.length = (uint32_t)fnode->size;
        }
    }
    odfs_timestamp_from_mac_time(hfs_be32(&data[16]), &fnode->mtime);
    fnode->ctime = fnode->mtime;
    return 1;
}

/* ------------------------------------------------------------------ */
/* mount                                                               */
/* ------------------------------------------------------------------ */

/* volume name from the root folder's thread record */
static odfs_err_t hfsp_root_thread_cb(const uint8_t *key, size_t key_len,
                                      const uint8_t *data, size_t data_len,
                                      void *cb_ctx)
{
    hfs_volume_t *vol = cb_ctx;
    uint16_t tnlen;

    (void)key;
    (void)key_len;

    /* thread record: type(2) reserved(2) parentID(4) namelen(2) name(...) */
    if (data_len < 10 || hfs_be16(data) != HFS_CAT_DTHREAD)
        return ODFS_OK;

    tnlen = hfs_be16(&data[8]);
    if (tnlen > 0 && tnlen <= (data_len - 10) / 2)
        hfsp_decode_name(&data[10], tnlen,
                         vol->volume_name, HFSPLUS_VOLNAME_SIZE);
    return ODFS_ERR_EOF;
}

static odfs_err_t hfsp_mount(odfs_cache_t *cache,
                               odfs_log_state_t *log,
                               uint32_t session_start,
                               odfs_node_t *root_out,
                               void **backend_ctx)
{
    hfs_volume_t *vol;
    uint64_t vh_offset;
    odfs_err_t err;

    vol = odfs_calloc(1, sizeof(*vol) + HFSPLUS_VOLNAME_SIZE);
    if (!vol) return ODFS_ERR_NOMEM;
    vol->hfsplus = 1;
    vol->parse_entry = hfsp_parse_entry;

    err = hfsp_find_vh(cache, session_start, &vh_offset);
    if (err != ODFS_OK) { odfs_free(vol); return err; }

    /* volume start is 1024 bytes before VH */
    vol->alloc_base = vh_offset - HFSPLUS_VH_OFFSET;

    /* read Volume Header (512 bytes) */
    uint8_t vh[512];
    {
        uint32_t lba = (uint32_t)(vh_offset / 2048);
        uint32_t off = (uint32_t)(vh_offset % 2048);
        const uint8_t *sector;
        err = odfs_cache_read(cache, lba, &sector);
        if (err != ODFS_OK) { odfs_free(vol); return err; }
        /* VH might span two sectors if misaligned */
        size_t avail = 2048 - off;
        if (avail >= 512) {
            memcpy(vh, sector + off, 512);
        } else {
            memcpy(vh, sector + off, avail);
            err = odfs_cache_read(cache, lba + 1, &sector);
            if (err != ODFS_OK) { odfs_free(vol); return err; }
            memcpy(vh + avail, sector, 512 - avail);
        }
    }

    uint16_t sig = hfs_be16(&vh[0]);
    if (sig != HFSPLUS_SIG && sig != HFSX_SIG) {
        odfs_free(vol); return ODFS_ERR_BAD_FORMAT;
    }

    vol->block_size   = hfs_be32(&vh[40]);
    vol->total_blocks = hfs_be32(&vh[44]);

    if (vol->block_size == 0 || vol->block_size > 65536) {
        odfs_free(vol); return ODFS_ERR_BAD_FORMAT;
    }

    ODFS_INFO(log, ODFS_SUB_HFSPLUS,
               "HFS+ block size: %" PRIu32 ", total: %" PRIu32,
               vol->block_size, vol->total_blocks);

    /* catalog fork at VH offset 272 (80 bytes): logicalSize(8)
       clumpSize(4) totalBlocks(4) extents[8] */
    for (int i = 0; i < 8; i++) {
        vol->cat.extents[i].start_block = hfs_be32(&vh[272 + 16 + i * 8]);
        vol->cat.extents[i].block_count = hfs_be32(&vh[272 + 20 + i * 8]);
    }

    /* read catalog B-Tree header node */
    err = odfs_hfs_init_btree(vol, &vol->cat, cache);
    if (err != ODFS_OK) {
        if (err == ODFS_ERR_BAD_FORMAT)
            ODFS_ERROR(log, ODFS_SUB_HFSPLUS, "bad catalog B-tree header");
        odfs_free(vol);
        return err;
    }

    err = odfs_hfs_alloc_node_buf(vol);
    if (err != ODFS_OK) {
        odfs_free(vol);
        return err;
    }

    ODFS_INFO(log, ODFS_SUB_HFSPLUS,
               "catalog: root %" PRIu32 ", node size %" PRIu32,
               vol->cat.root_node, vol->cat.node_size);

    /* try to get volume name from the catalog (root folder thread) */
    (void)odfs_hfs_walk_catalog(vol, cache, HFS_CNID_ROOT_DIR,
                                hfsp_root_thread_cb, vol);
    if (vol->volume_name[0] == '\0')
        memcpy(vol->volume_name, "HFS+", 5);

    ODFS_INFO(log, ODFS_SUB_HFSPLUS,
               "volume: \"%s\"", vol->volume_name);

    /* build root node */
    memset(root_out, 0, sizeof(*root_out));
    root_out->id = 0;
    root_out->parent_id = 0;
    root_out->backend = ODFS_BACKEND_HFSPLUS;
    root_out->kind = ODFS_NODE_DIR;
    root_out->name[0] = '/';
    root_out->name[1] = '\0';
    root_out->extent.lba = HFS_CNID_ROOT_DIR;
    root_out->extent.length = 0;

    odfs_timestamp_from_mac_time(hfs_be32(&vh[20]),
                                 &root_out->mtime);
    root_out->ctime = root_out->mtime;
    vol->root = *root_out;

    *backend_ctx = vol;
    return ODFS_OK;
}

/* ------------------------------------------------------------------ */
/* backend ops                                                         */
/* ------------------------------------------------------------------ */

const odfs_backend_ops_t hfsplus_backend_ops = {
    .name            = "hfsplus",
    .backend_type    = ODFS_BACKEND_HFSPLUS,
    .probe           = hfsp_probe,
    .mount           = hfsp_mount,
    .unmount         = odfs_hfs_unmount,
    .readdir         = odfs_hfs_readdir,
    .read            = odfs_hfs_read,
    .lookup          = odfs_hfs_lookup,
    .resolve_parent  = odfs_hfs_resolve_parent,
    .get_volume_name = odfs_hfs_get_volume_name,
    .get_volume_size = odfs_hfs_get_volume_size,
};
