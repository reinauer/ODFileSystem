/*
 * hfs.c — HFS backend (read-only)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Reads classic Macintosh HFS volumes, commonly found on hybrid
 * ISO/HFS CDs from the 1990s. Data fork only (resource forks are
 * not exposed). B-tree catalog traversal for directory listing.
 *
 * References: Inside Macintosh: Files, Linux fs/hfs/, libhfs
 */

#include "hfs.h"
#include "odfs/alloc.h"
#include "odfs/cache.h"
#include "odfs/charset.h"
#include "odfs/log.h"
#include "odfs/namefix.h"
#include "odfs/error.h"
#include "odfs/string.h"

#include <string.h>
#include <inttypes.h>

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

/* Mac epoch: 1904-01-01. Difference to Unix epoch in seconds. */
#define HFS_MAC_EPOCH_DIFF  2082844800UL

static void hfs_parse_mac_date(uint32_t mac_secs, odfs_timestamp_t *ts)
{
    /* rough conversion: subtract Mac epoch offset, then break into y/m/d */
    memset(ts, 0, sizeof(*ts));
    if (mac_secs < HFS_MAC_EPOCH_DIFF)
        return;

    uint32_t unix_secs = mac_secs - HFS_MAC_EPOCH_DIFF;
    /* Amiga epoch is 1978, but we store in our timestamp struct which
       has a year field. Use a simple days-from-epoch conversion. */
    uint32_t days = unix_secs / 86400;
    uint32_t rem = unix_secs % 86400;
    ts->hour = rem / 3600;
    ts->minute = (rem % 3600) / 60;
    ts->second = rem % 60;

    /* year/month/day from days since 1970-01-01 */
    int y = 1970;
    while (1) {
        int yd = 365 + ((y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 1 : 0);
        if (days < (uint32_t)yd) break;
        days -= yd;
        y++;
    }
    ts->year = y;

    static const int mdays[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 1 : 0;
    int m;
    for (m = 0; m < 12; m++) {
        int md = mdays[m] + (m == 1 ? leap : 0);
        if (days < (uint32_t)md) break;
        days -= md;
    }
    ts->month = m + 1;
    ts->day = days + 1;
}

/* convert allocation block number to physical byte offset */
static uint64_t hfs_ab_to_byte(const hfs_context_t *ctx, uint16_t ab)
{
    uint64_t block_512 = (uint64_t)ab * (ctx->alloc_block_size / 512)
                         + ctx->alloc_block_start
                         + ctx->vol_start_512;
    return block_512 * 512;
}

/* read bytes from the catalog B-tree at a given node number */
static odfs_err_t hfs_read_node(const hfs_context_t *ctx,
                                  odfs_cache_t *cache,
                                  uint32_t node_num,
                                  uint8_t *buf)
{
    /* calculate byte offset of node within catalog file */
    uint64_t node_byte_offset = (uint64_t)node_num * ctx->cat_node_size;

    /* find which extent contains this offset */
    uint64_t extent_offset = 0;
    for (int i = 0; i < 3; i++) {
        uint64_t ext_bytes = (uint64_t)ctx->cat_extents[i].num_ab * ctx->alloc_block_size;
        if (node_byte_offset < extent_offset + ext_bytes) {
            uint64_t off_in_ext = node_byte_offset - extent_offset;
            uint64_t phys = hfs_ab_to_byte(ctx, ctx->cat_extents[i].start_ab) + off_in_ext;
            uint32_t lba = (uint32_t)(phys / 2048);
            uint32_t lba_off = (uint32_t)(phys % 2048);

            /* read enough 2048-byte sectors to cover the node */
            uint32_t remain = ctx->cat_node_size;
            uint32_t buf_pos = 0;
            while (remain > 0) {
                const uint8_t *sector;
                odfs_err_t err = odfs_cache_read(cache, lba, &sector);
                if (err != ODFS_OK) return err;

                uint32_t chunk = 2048 - lba_off;
                if (chunk > remain) chunk = remain;
                memcpy(buf + buf_pos, sector + lba_off, chunk);
                buf_pos += chunk;
                remain -= chunk;
                lba++;
                lba_off = 0;
            }
            return ODFS_OK;
        }
        extent_offset += ext_bytes;
    }
    return ODFS_ERR_RANGE;
}

/* get record offset from the offset table at the end of a node */
static uint16_t hfs_rec_offset(const uint8_t *node, uint32_t node_size,
                                int rec_idx)
{
    /* offset table is at end of node, growing backward: 2 bytes per entry */
    uint32_t pos = node_size - 2 * (rec_idx + 1);
    return hfs_be16(&node[pos]);
}

static int hfs_read_record_count(const uint8_t *node, uint32_t node_size,
                                 uint16_t *nrecs_out)
{
    uint16_t max_recs;
    uint16_t nrecs;

    if (node_size < 12)
        return 0;

    /* Reserve one trailing offset-table slot for r + 1 lookups. */
    max_recs = (uint16_t)((node_size / 2) - 1);
    nrecs = hfs_be16(&node[10]);
    if (nrecs > max_recs)
        return 0;

    *nrecs_out = nrecs;
    return 1;
}

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
/* mount                                                               */
/* ------------------------------------------------------------------ */

static odfs_err_t hfs_mount(odfs_cache_t *cache,
                              odfs_log_state_t *log,
                              uint32_t session_start,
                              odfs_node_t *root_out,
                              void **backend_ctx)
{
    hfs_context_t *ctx;
    const uint8_t *sector;
    const uint8_t *mdb;
    odfs_err_t err;
    uint32_t vol_start_512 = 0;

    ctx = odfs_calloc(1, sizeof(*ctx));
    if (!ctx) return ODFS_ERR_NOMEM;

    ctx->next_node_id = 1;

    /* find MDB — try direct first, then APM */
    err = odfs_cache_read(cache, session_start, &sector);
    if (err != ODFS_OK) { odfs_free(ctx); return err; }

    if (hfs_be16(&sector[1024]) == HFS_MDB_SIG) {
        vol_start_512 = session_start * 4; /* 2048/512 = 4 */
        mdb = &sector[1024];
    } else if (hfs_be16(&sector[512]) == HFS_APM_SIG) {
        const uint8_t *pm = &sector[512];
        if (memcmp(&pm[48], "Apple_HFS", 9) != 0) {
            odfs_free(ctx); return ODFS_ERR_BAD_FORMAT;
        }
        uint32_t pstart = hfs_be32(&pm[8]);
        vol_start_512 = session_start * 4 + pstart;
        uint64_t mdb_byte = ((uint64_t)vol_start_512 + 2) * 512;
        uint32_t mdb_lba = (uint32_t)(mdb_byte / 2048);
        uint32_t mdb_off = (uint32_t)(mdb_byte % 2048);
        err = odfs_cache_read(cache, mdb_lba, &sector);
        if (err != ODFS_OK) { odfs_free(ctx); return err; }
        mdb = &sector[mdb_off];
        if (hfs_be16(mdb) != HFS_MDB_SIG) { odfs_free(ctx); return ODFS_ERR_BAD_FORMAT; }
    } else {
        odfs_free(ctx); return ODFS_ERR_BAD_FORMAT;
    }

    ctx->vol_start_512 = vol_start_512;

    /* parse MDB */
    ctx->num_alloc_blocks = hfs_be16(&mdb[HFS_MDB_NMALBLKS]);
    ctx->alloc_block_size = hfs_be32(&mdb[HFS_MDB_ALBLKSIZ]);
    ctx->alloc_block_start = hfs_be16(&mdb[HFS_MDB_ALBLST]);

    uint8_t volname_raw[27];
    memcpy(volname_raw, &mdb[HFS_MDB_VOLNAME], sizeof(volname_raw));
    uint8_t vnamelen = mdb[HFS_MDB_VOLNAMELEN];
    if (vnamelen > 27) vnamelen = 27;
    hfs_name_to_utf8(volname_raw, vnamelen,
                     ctx->volume_name, sizeof(ctx->volume_name));

    ctx->cat_file_size = hfs_be32(&mdb[HFS_MDB_CTFLSIZE]);
    for (int i = 0; i < 3; i++) {
        ctx->cat_extents[i].start_ab = hfs_be16(&mdb[HFS_MDB_CTEXTREC + i * 4]);
        ctx->cat_extents[i].num_ab   = hfs_be16(&mdb[HFS_MDB_CTEXTREC + i * 4 + 2]);
    }

    ODFS_INFO(log, ODFS_SUB_HFS,
               "volume: \"%s\", alloc blocks: %u, block size: %" PRIu32,
               ctx->volume_name, ctx->num_alloc_blocks, ctx->alloc_block_size);

    /* read catalog B-tree header node */
    {
        uint8_t hdr_buf[512];
        err = hfs_read_node(ctx, cache, 0, hdr_buf);
        if (err != ODFS_OK) { odfs_free(ctx); return err; }

        if (hdr_buf[8] != HFS_NODE_HEADER) {
            ODFS_ERROR(log, ODFS_SUB_HFS, "catalog header node type %u", hdr_buf[8]);
            odfs_free(ctx); return ODFS_ERR_BAD_FORMAT;
        }

        ctx->cat_root_node = hfs_be32(&hdr_buf[16]);
        ctx->cat_first_leaf = hfs_be32(&hdr_buf[24]);
        ctx->cat_node_size = hfs_be16(&hdr_buf[32]);

        if (ctx->cat_node_size < 14 || ctx->cat_node_size > 32768) {
            odfs_free(ctx); return ODFS_ERR_BAD_FORMAT;
        }

        ODFS_INFO(log, ODFS_SUB_HFS,
                   "catalog: root node %" PRIu32 ", node size %" PRIu32,
                   ctx->cat_root_node, ctx->cat_node_size);
    }

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

    /* parse MDB modification date for root timestamp */
    hfs_parse_mac_date(hfs_be32(&mdb[HFS_MDB_LSMOD]), &root_out->mtime);
    root_out->ctime = root_out->mtime;

    *backend_ctx = ctx;
    return ODFS_OK;
}

static void hfs_unmount(void *backend_ctx)
{
    odfs_free(backend_ctx);
}

/* ------------------------------------------------------------------ */
/* catalog B-tree leaf scan                                            */
/* ------------------------------------------------------------------ */

/*
 * Walk catalog leaf nodes, calling the callback for each entry
 * matching the target parent CNID. The B-tree is sorted by
 * (parentID, name), so all entries for a directory are contiguous.
 */

typedef odfs_err_t (*hfs_cat_cb)(const uint8_t *key, size_t key_len,
                                   const uint8_t *data, size_t data_len,
                                   void *cb_ctx);

static odfs_err_t hfs_walk_catalog(hfs_context_t *ctx,
                                     odfs_cache_t *cache,
                                     uint32_t parent_cnid,
                                     hfs_cat_cb callback,
                                     void *cb_ctx)
{
    uint8_t *node_buf;
    odfs_err_t err;
    uint32_t cur_node;
    int found_start = 0;

    node_buf = odfs_malloc(ctx->cat_node_size);
    if (!node_buf) return ODFS_ERR_NOMEM;

    /* start from root and descend to the right leaf */
    cur_node = ctx->cat_root_node;

    /* descend index nodes (max depth ~10) */
    for (int depth = 0; depth < 20; depth++) {
        err = hfs_read_node(ctx, cache, cur_node, node_buf);
        if (err != ODFS_OK) { odfs_free(node_buf); return err; }

        uint8_t ntype = node_buf[8];
        uint16_t nrecs;

        if (ntype == HFS_NODE_LEAF)
            break; /* reached leaf level */

        if (ntype != HFS_NODE_INDEX) {
            odfs_free(node_buf);
            return ODFS_ERR_CORRUPT;
        }

        if (!hfs_read_record_count(node_buf, ctx->cat_node_size, &nrecs)) {
            odfs_free(node_buf);
            return ODFS_ERR_CORRUPT;
        }

        /* find the right child in the index node */
        uint32_t child = 0;
        for (uint16_t r = 0; r < nrecs; r++) {
            uint32_t off = hfs_rec_offset(node_buf, ctx->cat_node_size, r);
            if (off + 6 >= ctx->cat_node_size) break;

            uint8_t klen = node_buf[off];
            if (off + (uint32_t)klen + 1 + 4 >= ctx->cat_node_size) break;

            uint32_t key_parent = hfs_be32(&node_buf[off + 2]);
            uint32_t ptr = hfs_be32(&node_buf[off + 1 + klen]);

            if (key_parent <= parent_cnid)
                child = ptr;
            else
                break;
        }

        if (child == 0) { odfs_free(node_buf); return ODFS_ERR_NOT_FOUND; }
        cur_node = child;
    }

    /* now scan leaf nodes for entries matching parent_cnid */
    while (cur_node != 0) {
        err = hfs_read_node(ctx, cache, cur_node, node_buf);
        if (err != ODFS_OK) { odfs_free(node_buf); return err; }

        if (node_buf[8] != HFS_NODE_LEAF) break;

        uint16_t nrecs;
        if (!hfs_read_record_count(node_buf, ctx->cat_node_size, &nrecs)) {
            odfs_free(node_buf);
            return ODFS_ERR_CORRUPT;
        }

        for (uint16_t r = 0; r < nrecs; r++) {
            uint32_t off = hfs_rec_offset(node_buf, ctx->cat_node_size, r);
            uint32_t next_off = hfs_rec_offset(node_buf, ctx->cat_node_size, r + 1);

            if (off + 6 >= ctx->cat_node_size) continue;

            uint8_t klen = node_buf[off];
            if (klen < 5 || off + 1 + (uint32_t)klen >= ctx->cat_node_size) continue;

            uint32_t key_parent = hfs_be32(&node_buf[off + 2]);

            if (key_parent < parent_cnid) continue;
            if (key_parent > parent_cnid) {
                /* past our directory — done */
                odfs_free(node_buf);
                return ODFS_OK;
            }

            found_start = 1;

            /* record data follows the key (padded to even boundary) */
            uint16_t data_off = off + 1 + klen;
            if (data_off & 1) data_off++; /* word-align */
            uint16_t data_len = (next_off > data_off) ? next_off - data_off : 0;

            if (data_off < ctx->cat_node_size && data_len > 0) {
                err = callback(&node_buf[off], klen + 1,
                               &node_buf[data_off], data_len, cb_ctx);
                if (err != ODFS_OK) {
                    odfs_free(node_buf);
                    return err;
                }
            }
        }

        /* follow forward link to next leaf */
        cur_node = hfs_be32(&node_buf[0]);
        if (found_start && cur_node != 0) {
            /* peek at next node's first key to see if we're done */
            err = hfs_read_node(ctx, cache, cur_node, node_buf);
            if (err != ODFS_OK) { odfs_free(node_buf); return err; }
            if (node_buf[8] != HFS_NODE_LEAF) break;
            uint32_t off = hfs_rec_offset(node_buf, ctx->cat_node_size, 0);
            if (off + 6 < ctx->cat_node_size) {
                uint32_t next_parent = hfs_be32(&node_buf[off + 2]);
                if (next_parent != parent_cnid) break;
            }
            /* reset cur_node to re-process this node from the loop */
            /* actually we already read it, just continue the while loop */
            continue;
        }
    }

    odfs_free(node_buf);
    return ODFS_OK;
}

/* ------------------------------------------------------------------ */
/* readdir                                                             */
/* ------------------------------------------------------------------ */

typedef struct hfs_readdir_ctx {
    hfs_context_t *hctx;
    odfs_cache_t *cache;
    const odfs_node_t *dir;
    odfs_dir_iter_fn callback;
    void *cb_ctx;
    uint32_t entry_index;
    uint32_t skip_to;
    uint32_t *resume_offset;
    odfs_err_t last_err;
    odfs_namefix_state_t namefix;
} hfs_readdir_ctx_t;

static odfs_err_t hfs_readdir_cb(const uint8_t *key, size_t key_len,
                                    const uint8_t *data, size_t data_len,
                                    void *cb_ctx)
{
    hfs_readdir_ctx_t *rc = cb_ctx;

    if (data_len < 1) return ODFS_OK;

    uint8_t rec_type = data[0];

    /* only interested in file and directory records, not threads */
    if (rec_type != HFS_CAT_FILE && rec_type != HFS_CAT_DIR)
        return ODFS_OK;

    odfs_node_t node;
    memset(&node, 0, sizeof(node));
    node.id = rc->hctx->next_node_id++;
    node.parent_id = rc->dir->id;
    node.backend = ODFS_BACKEND_HFS;

    /* extract name from catalog key:
       key[0] = key length, key[1] = reserved, key[2..5] = parentID,
       key[6] = name length, key[7..] = name bytes */
    if (key_len >= 8) {
        uint8_t nlen = key[6];
        if (nlen > 0 && (size_t)(7 + nlen) <= key_len) {
            hfs_name_to_utf8(&key[7], nlen, node.name, sizeof(node.name));
        }
    }

    if (rec_type == HFS_CAT_DIR && data_len >= 22) {
        node.kind = ODFS_NODE_DIR;
        uint32_t dir_id = hfs_be32(&data[6]);
        node.extent.lba = dir_id; /* store CNID for readdir */
        hfs_parse_mac_date(hfs_be32(&data[14]), &node.mtime);
    } else if (rec_type == HFS_CAT_FILE && data_len >= 52) {
        node.kind = ODFS_NODE_FILE;
        node.size = hfs_be32(&data[26]); /* data fork logical length */
        /* data fork first extent: at offset 74 in file record */
        if (data_len >= 78) {
            node.extent.lba = hfs_be16(&data[74]);    /* first extent start AB */
            node.extent.length = (uint32_t)node.size;
        }
        hfs_parse_mac_date(hfs_be32(&data[48]), &node.mtime);
    }
    node.ctime = node.mtime;

    odfs_err_t err = odfs_namefix_apply(&rc->namefix, node.name, sizeof(node.name));
    if (err != ODFS_OK) {
        rc->last_err = err;
        return err;
    }

    /* skip to resume point */
    if (rc->entry_index < rc->skip_to) {
        rc->entry_index++;
        return ODFS_OK;
    }

    rc->entry_index++;

    rc->last_err = rc->callback(&node, rc->cb_ctx);
    if (rc->last_err != ODFS_OK) {
        if (rc->resume_offset)
            *rc->resume_offset = rc->entry_index;
        return rc->last_err;
    }

    return ODFS_OK;
}

static odfs_err_t hfs_readdir(void *backend_ctx,
                                odfs_cache_t *cache,
                                odfs_log_state_t *log,
                                const odfs_node_t *dir,
                                odfs_dir_iter_fn callback,
                                void *cb_ctx,
                                uint32_t *resume_offset)
{
    hfs_context_t *ctx = backend_ctx;
    (void)log;

    /* dir->extent.lba stores the directory's CNID */
    uint32_t parent_cnid = dir->extent.lba;

    hfs_readdir_ctx_t rc;
    rc.hctx = ctx;
    rc.cache = cache;
    rc.dir = dir;
    rc.callback = callback;
    rc.cb_ctx = cb_ctx;
    rc.entry_index = 0;
    rc.skip_to = (resume_offset && *resume_offset) ? *resume_offset : 0;
    rc.resume_offset = resume_offset;
    rc.last_err = ODFS_OK;
    odfs_namefix_init(&rc.namefix);

    odfs_err_t err = hfs_walk_catalog(ctx, cache, parent_cnid,
                                        hfs_readdir_cb, &rc);
    odfs_namefix_destroy(&rc.namefix);
    if (rc.last_err != ODFS_OK)
        return rc.last_err;

    if (resume_offset)
        *resume_offset = rc.entry_index; /* exhausted */
    return err;
}

/* ------------------------------------------------------------------ */
/* read                                                                */
/* ------------------------------------------------------------------ */

static odfs_err_t hfs_read(void *backend_ctx,
                             odfs_cache_t *cache,
                             odfs_log_state_t *log,
                             const odfs_node_t *file,
                             uint64_t offset,
                             void *buf,
                             size_t *len)
{
    hfs_context_t *ctx = backend_ctx;
    (void)log;

    if (offset >= file->size) { *len = 0; return ODFS_OK; }

    size_t want = *len;
    if (offset + want > file->size)
        want = (size_t)(file->size - offset);

    /* file->extent.lba = first extent start allocation block */
    uint64_t data_start = hfs_ab_to_byte(ctx, (uint16_t)file->extent.lba);

    size_t done = 0;
    uint8_t *out = buf;

    while (done < want) {
        uint64_t pos = data_start + offset + done;
        uint32_t lba = (uint32_t)(pos / 2048);
        uint32_t lba_off = (uint32_t)(pos % 2048);
        const uint8_t *sector;

        odfs_err_t err = odfs_cache_read(cache, lba, &sector);
        if (err != ODFS_OK) { *len = done; return err; }

        size_t chunk = 2048 - lba_off;
        if (chunk > want - done) chunk = want - done;

        memcpy(out + done, sector + lba_off, chunk);
        done += chunk;
    }

    *len = done;
    return ODFS_OK;
}

/* ------------------------------------------------------------------ */
/* lookup                                                              */
/* ------------------------------------------------------------------ */

typedef struct hfs_lookup_ctx {
    const char   *name;
    odfs_node_t *result;
    int           found;
} hfs_lookup_ctx_t;

static odfs_err_t hfs_lookup_iter(const odfs_node_t *entry, void *ctx)
{
    hfs_lookup_ctx_t *lc = ctx;
    if (odfs_strcasecmp(entry->name, lc->name) == 0) {
        *lc->result = *entry;
        lc->found = 1;
        return ODFS_ERR_EOF;
    }
    return ODFS_OK;
}

static odfs_err_t hfs_lookup(void *backend_ctx,
                               odfs_cache_t *cache,
                               odfs_log_state_t *log,
                               const odfs_node_t *dir,
                               const char *name,
                               odfs_node_t *out)
{
    hfs_lookup_ctx_t lc;
    lc.name = name;
    lc.result = out;
    lc.found = 0;

    odfs_err_t err = hfs_readdir(backend_ctx, cache, log, dir,
                                   hfs_lookup_iter, &lc, NULL);
    if (err == ODFS_ERR_EOF && lc.found) return ODFS_OK;
    if (err != ODFS_OK) return err;
    if (lc.found) return ODFS_OK;
    return ODFS_ERR_NOT_FOUND;
}

/* ------------------------------------------------------------------ */
/* get_volume_name                                                     */
/* ------------------------------------------------------------------ */

static odfs_err_t hfs_get_volume_name(void *backend_ctx,
                                        char *buf, size_t buf_size)
{
    hfs_context_t *ctx = backend_ctx;
    size_t len = strlen(ctx->volume_name);
    if (len >= buf_size) len = buf_size - 1;
    memcpy(buf, ctx->volume_name, len);
    buf[len] = '\0';
    return ODFS_OK;
}

/* ------------------------------------------------------------------ */
/* get_volume_size                                                     */
/* ------------------------------------------------------------------ */

static uint32_t hfs_get_volume_size(void *backend_ctx)
{
    hfs_context_t *ctx = backend_ctx;
    uint64_t bytes = (uint64_t)ctx->num_alloc_blocks * ctx->alloc_block_size;
    uint64_t blocks = (bytes + 2047u) / 2048u;

    return blocks > UINT32_MAX ? UINT32_MAX : (uint32_t)blocks;
}

/* ------------------------------------------------------------------ */
/* backend ops table                                                   */
/* ------------------------------------------------------------------ */

const odfs_backend_ops_t hfs_backend_ops = {
    .name            = "hfs",
    .backend_type    = ODFS_BACKEND_HFS,
    .probe           = hfs_probe,
    .mount           = hfs_mount,
    .unmount         = hfs_unmount,
    .readdir         = hfs_readdir,
    .read            = hfs_read,
    .lookup          = hfs_lookup,
    .get_volume_name = hfs_get_volume_name,
    .get_volume_size = hfs_get_volume_size,
};
