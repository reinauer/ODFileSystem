/*
 * hfsplus.c — HFS+ backend (read-only)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Reads HFS+ volumes. Data fork only. Handles volumes with or
 * without GPT/APM partition wrappers by scanning for the 'H+'
 * signature. B-Tree nodes are typically 4096 bytes.
 *
 * References: Apple TN1150, Linux fs/hfsplus/
 */

#include "hfsplus.h"
#include "odfs/cache.h"
#include "odfs/charset.h"
#include "odfs/log.h"
#include "odfs/error.h"

#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

/* Mac epoch offset: seconds from 1904-01-01 to 1970-01-01 */
#define HFSP_MAC_EPOCH  2082844800UL

static void hfsp_parse_date(uint32_t mac_secs, odfs_timestamp_t *ts)
{
    memset(ts, 0, sizeof(*ts));
    if (mac_secs < HFSP_MAC_EPOCH)
        return;
    uint32_t unix_secs = mac_secs - HFSP_MAC_EPOCH;
    uint32_t days = unix_secs / 86400;
    uint32_t rem = unix_secs % 86400;
    ts->hour = rem / 3600;
    ts->minute = (rem % 3600) / 60;
    ts->second = rem % 60;

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

/* parse fork data from 80 bytes on disc */
static void hfsp_parse_fork(const uint8_t *p, hfsp_fork_t *f)
{
    f->logical_size = hfsp_be64(&p[0]);
    f->clump_size   = hfsp_be32(&p[8]);
    f->total_blocks = hfsp_be32(&p[12]);
    for (int i = 0; i < 8; i++) {
        f->extents[i].start_block = hfsp_be32(&p[16 + i * 8]);
        f->extents[i].block_count = hfsp_be32(&p[16 + i * 8 + 4]);
    }
}

/* convert allocation block to byte offset in image */
static uint64_t hfsp_block_to_byte(const hfsplus_context_t *ctx, uint32_t block)
{
    return ctx->vol_offset + (uint64_t)block * ctx->block_size;
}

/* read bytes from a fork at a given byte offset within the fork */
static odfs_err_t hfsp_read_fork(const hfsplus_context_t *ctx,
                                   odfs_cache_t *cache,
                                   const hfsp_fork_t *fork,
                                   uint64_t offset,
                                   uint8_t *buf, size_t len)
{
    size_t done = 0;
    uint64_t ext_offset = 0;

    for (int i = 0; i < 8 && done < len; i++) {
        uint64_t ext_bytes = (uint64_t)fork->extents[i].block_count * ctx->block_size;
        if (ext_bytes == 0) break;

        if (offset < ext_offset + ext_bytes) {
            uint64_t off_in_ext = offset - ext_offset;
            uint64_t phys = hfsp_block_to_byte(ctx, fork->extents[i].start_block) + off_in_ext;

            while (done < len && off_in_ext < ext_bytes) {
                uint32_t lba = (uint32_t)(phys / 2048);
                uint32_t lba_off = (uint32_t)(phys % 2048);
                const uint8_t *sector;

                odfs_err_t err = odfs_cache_read(cache, lba, &sector);
                if (err != ODFS_OK) return err;

                size_t chunk = 2048 - lba_off;
                if (chunk > len - done) chunk = len - done;
                if (chunk > ext_bytes - off_in_ext) chunk = (size_t)(ext_bytes - off_in_ext);

                memcpy(buf + done, sector + lba_off, chunk);
                done += chunk;
                off_in_ext += chunk;
                phys += chunk;
                offset += chunk;
            }
        }
        ext_offset += ext_bytes;
    }

    return (done == len) ? ODFS_OK : ODFS_ERR_IO;
}

/* read a B-Tree node */
static odfs_err_t hfsp_read_node(const hfsplus_context_t *ctx,
                                   odfs_cache_t *cache,
                                   uint32_t node_num,
                                   uint8_t *buf)
{
    uint64_t offset = (uint64_t)node_num * ctx->cat_node_size;
    return hfsp_read_fork(ctx, cache, &ctx->cat_fork, offset,
                          buf, ctx->cat_node_size);
}

/* get record offset from the offset table at end of node */
static uint16_t hfsp_rec_offset(const uint8_t *node, uint32_t node_size,
                                 int rec_idx)
{
    uint32_t pos = node_size - 2 * (rec_idx + 1);
    return hfsp_be16(&node[pos]);
}

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

static odfs_err_t hfsp_find_vh(odfs_cache_t *cache,
                                 uint32_t session_start,
                                 uint64_t *vh_offset_out)
{
    /*
     * Try common locations for the Volume Header:
     * 1. Byte 1024 from image start (bare HFS+ volume)
     * 2. Scan 512-byte boundaries in first 256K for 'H+' or 'HX'
     *    (handles GPT/APM partitioned images)
     */
    const uint8_t *sector;
    uint64_t base = (uint64_t)session_start * 2048;

    /* try byte 1024 (within first 2048-byte sector) */
    if (odfs_cache_read(cache, session_start, &sector) == ODFS_OK) {
        uint16_t sig = hfsp_be16(&sector[1024]);
        if (sig == HFSPLUS_SIG || sig == HFSX_SIG) {
            *vh_offset_out = base + 1024;
            return ODFS_OK;
        }
    }

    /* scan first 256K for the signature */
    for (uint32_t lba = session_start; lba < session_start + 128; lba++) {
        if (odfs_cache_read(cache, lba, &sector) != ODFS_OK)
            continue;
        for (int off = 0; off <= 2048 - 2; off += 512) {
            uint16_t sig = hfsp_be16(&sector[off]);
            if (sig == HFSPLUS_SIG || sig == HFSX_SIG) {
                /* VH is at this offset; volume start is 1024 before it */
                uint64_t vh_byte = (uint64_t)lba * 2048 + off;
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
/* mount                                                               */
/* ------------------------------------------------------------------ */

static odfs_err_t hfsp_mount(odfs_cache_t *cache,
                               odfs_log_state_t *log,
                               uint32_t session_start,
                               odfs_node_t *root_out,
                               void **backend_ctx)
{
    hfsplus_context_t *ctx;
    uint64_t vh_offset;
    odfs_err_t err;

    ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return ODFS_ERR_NOMEM;
    ctx->next_node_id = 1;

    err = hfsp_find_vh(cache, session_start, &vh_offset);
    if (err != ODFS_OK) { free(ctx); return err; }

    /* volume start is 1024 bytes before VH */
    ctx->vol_offset = vh_offset - HFSPLUS_VH_OFFSET;

    /* read Volume Header (512 bytes) */
    uint8_t vh[512];
    {
        uint32_t lba = (uint32_t)(vh_offset / 2048);
        uint32_t off = (uint32_t)(vh_offset % 2048);
        const uint8_t *sector;
        err = odfs_cache_read(cache, lba, &sector);
        if (err != ODFS_OK) { free(ctx); return err; }
        /* VH might span two sectors if misaligned */
        size_t avail = 2048 - off;
        if (avail >= 512) {
            memcpy(vh, sector + off, 512);
        } else {
            memcpy(vh, sector + off, avail);
            err = odfs_cache_read(cache, lba + 1, &sector);
            if (err != ODFS_OK) { free(ctx); return err; }
            memcpy(vh + avail, sector, 512 - avail);
        }
    }

    uint16_t sig = hfsp_be16(&vh[0]);
    if (sig != HFSPLUS_SIG && sig != HFSX_SIG) {
        free(ctx); return ODFS_ERR_BAD_FORMAT;
    }

    ctx->block_size   = hfsp_be32(&vh[40]);
    ctx->total_blocks = hfsp_be32(&vh[44]);

    if (ctx->block_size == 0 || ctx->block_size > 65536) {
        free(ctx); return ODFS_ERR_BAD_FORMAT;
    }

    ODFS_INFO(log, ODFS_SUB_HFSPLUS,
               "HFS+ block size: %" PRIu32 ", total: %" PRIu32,
               ctx->block_size, ctx->total_blocks);

    /* catalog fork at VH offset 272 (80 bytes) */
    hfsp_parse_fork(&vh[272], &ctx->cat_fork);

    /* read catalog B-Tree header node */
    {
        /* first, read just enough to get node size from header */
        uint8_t hdr_buf[512];
        /* read first 512 bytes of catalog file */
        err = hfsp_read_fork(ctx, cache, &ctx->cat_fork, 0, hdr_buf, 512);
        if (err != ODFS_OK) { free(ctx); return err; }

        if (hdr_buf[8] != 0x01) { /* node type: header */
            ODFS_ERROR(log, ODFS_SUB_HFSPLUS,
                        "catalog header node type %u", hdr_buf[8]);
            free(ctx); return ODFS_ERR_BAD_FORMAT;
        }

        /* header record at offset 14 in the node */
        /* B-Tree header record: depth(2) root(4) leafRecords(4)
           firstLeaf(4) lastLeaf(4) nodeSize(2) maxKeyLen(2) ... */
        ctx->cat_root_node  = hfsp_be32(&hdr_buf[14 + 2]);
        ctx->cat_first_leaf = hfsp_be32(&hdr_buf[14 + 2 + 4 + 4]);
        ctx->cat_node_size  = hfsp_be16(&hdr_buf[14 + 2 + 4 + 4 + 4 + 4]);

        if (ctx->cat_node_size == 0 || ctx->cat_node_size > 32768) {
            free(ctx); return ODFS_ERR_BAD_FORMAT;
        }

        ODFS_INFO(log, ODFS_SUB_HFSPLUS,
                   "catalog: root %" PRIu32 ", node size %" PRIu32,
                   ctx->cat_root_node, ctx->cat_node_size);
    }

    /* try to get volume name from the catalog (root folder thread) */
    {
        uint8_t *node = malloc(ctx->cat_node_size);
        if (node) {
            /* read root node and scan for folder thread of CNID 2 */
            err = hfsp_read_node(ctx, cache, ctx->cat_first_leaf, node);
            if (err == ODFS_OK && node[8] == 0xFF) { /* leaf */
                uint16_t nrecs = hfsp_be16(&node[10]);
                for (uint16_t r = 0; r < nrecs; r++) {
                    uint32_t roff = hfsp_rec_offset(node, ctx->cat_node_size, r);
                    if (roff + 10 >= ctx->cat_node_size) continue;
                    /* catalog key: keylen(2) parentID(4) namelen(2) name(...) */
                    uint32_t parent_cnid = hfsp_be32(&node[roff + 2]);
                    uint16_t namelen = hfsp_be16(&node[roff + 6]);
                    uint32_t keylen = hfsp_be16(&node[roff]);
                    uint32_t data_off = roff + 2 + keylen;
                    if (data_off + 2 >= ctx->cat_node_size) continue;
                    int16_t rec_type = (int16_t)hfsp_be16(&node[data_off]);
                    if (rec_type == HFSPLUS_FOLDER_THREAD && parent_cnid == HFSPLUS_CNID_ROOT) {
                        /* thread record: type(2) reserved(2) parentID(4) namelen(2) name(...) */
                        if (data_off + 8 + 2 < ctx->cat_node_size) {
                            uint16_t tnlen = hfsp_be16(&node[data_off + 8]);
                            if (tnlen > 0 && data_off + 10 + tnlen * 2 <= ctx->cat_node_size) {
                                hfsp_decode_name(&node[data_off + 10], tnlen,
                                                 ctx->volume_name, sizeof(ctx->volume_name));
                            }
                        }
                    }
                    (void)namelen;
                }
            }
            free(node);
        }
    }

    if (ctx->volume_name[0] == '\0')
        memcpy(ctx->volume_name, "HFS+", 5);

    ODFS_INFO(log, ODFS_SUB_HFSPLUS,
               "volume: \"%s\"", ctx->volume_name);

    /* build root node */
    memset(root_out, 0, sizeof(*root_out));
    root_out->id = 0;
    root_out->parent_id = 0;
    root_out->backend = ODFS_BACKEND_HFSPLUS;
    root_out->kind = ODFS_NODE_DIR;
    root_out->name[0] = '/';
    root_out->name[1] = '\0';
    root_out->extent.lba = HFSPLUS_CNID_ROOT;
    root_out->extent.length = 0;

    hfsp_parse_date(hfsp_be32(&vh[20]), &root_out->mtime);
    root_out->ctime = root_out->mtime;

    *backend_ctx = ctx;
    return ODFS_OK;
}

static void hfsp_unmount(void *backend_ctx)
{
    free(backend_ctx);
}

/* ------------------------------------------------------------------ */
/* readdir — walk catalog leaves for entries with matching parent CNID */
/* ------------------------------------------------------------------ */

static odfs_err_t hfsp_readdir(void *backend_ctx,
                                 odfs_cache_t *cache,
                                 odfs_log_state_t *log,
                                 const odfs_node_t *dir,
                                 odfs_dir_iter_fn callback,
                                 void *cb_ctx,
                                 uint32_t *resume_offset)
{
    hfsplus_context_t *ctx = backend_ctx;
    uint32_t parent_cnid = dir->extent.lba;
    uint8_t *node;
    odfs_err_t err;
    uint32_t cur_node;
    int entry_index = 0;
    int skip_to = (resume_offset && *resume_offset) ? (int)*resume_offset : 0;
    int found_parent = 0;
    (void)log;

    node = malloc(ctx->cat_node_size);
    if (!node) return ODFS_ERR_NOMEM;

    /* descend B-Tree from root to find the right leaf */
    cur_node = ctx->cat_root_node;
    for (int depth = 0; depth < 20; depth++) {
        err = hfsp_read_node(ctx, cache, cur_node, node);
        if (err != ODFS_OK) { free(node); return err; }

        uint8_t ntype = node[8];
        if (ntype == 0xFF) break; /* leaf */
        if (ntype != 0x00) { free(node); return ODFS_ERR_CORRUPT; } /* not index */

        uint16_t nrecs = hfsp_be16(&node[10]);
        uint32_t child = 0;
        for (uint16_t r = 0; r < nrecs; r++) {
            uint32_t roff = hfsp_rec_offset(node, ctx->cat_node_size, r);
            if (roff + 6 >= ctx->cat_node_size) break;
            uint32_t key_parent = hfsp_be32(&node[roff + 2]);
            uint32_t keylen = hfsp_be16(&node[roff]);
            uint32_t ptr_off = roff + 2 + keylen;
            if (ptr_off + 4 > ctx->cat_node_size) break;
            uint32_t ptr = hfsp_be32(&node[ptr_off]);
            if (key_parent <= parent_cnid)
                child = ptr;
            else
                break;
        }
        if (child == 0) { free(node); return ODFS_ERR_NOT_FOUND; }
        cur_node = child;
    }

    /* scan leaf nodes */
    while (cur_node != 0) {
        err = hfsp_read_node(ctx, cache, cur_node, node);
        if (err != ODFS_OK) { free(node); return err; }
        if (node[8] != 0xFF) break; /* not leaf */

        uint16_t nrecs = hfsp_be16(&node[10]);
        for (uint16_t r = 0; r < nrecs; r++) {
            uint32_t roff = hfsp_rec_offset(node, ctx->cat_node_size, r);
            uint32_t next_roff = hfsp_rec_offset(node, ctx->cat_node_size, r + 1);
            if (roff + 8 >= ctx->cat_node_size) continue;

            uint32_t keylen = hfsp_be16(&node[roff]);
            uint32_t key_parent = hfsp_be32(&node[roff + 2]);
            uint16_t key_namelen = hfsp_be16(&node[roff + 6]);

            if (key_parent < parent_cnid) continue;
            if (key_parent > parent_cnid) {
                free(node);
                if (resume_offset) *resume_offset = (uint32_t)entry_index;
                return ODFS_OK;
            }
            found_parent = 1;

            uint32_t data_off = roff + 2 + keylen;
            if (data_off & 1) data_off++;
            uint32_t data_len = (next_roff > data_off) ? next_roff - data_off : 0;
            if (data_off + 2 >= ctx->cat_node_size || data_len < 2) continue;

            int16_t rec_type = (int16_t)hfsp_be16(&node[data_off]);

            if (rec_type != HFSPLUS_FOLDER_REC && rec_type != HFSPLUS_FILE_REC)
                continue;

            /* skip entries with empty names and HFS+ private dirs */
            if (key_namelen == 0)
                continue;
            if (key_namelen >= 4 && node[roff + 8] == 0x00 && node[roff + 9] == 0x2E)
                continue; /* name starts with "." — skip private dirs */

            if (entry_index < skip_to) {
                entry_index++;
                continue;
            }

            odfs_node_t fnode;
            memset(&fnode, 0, sizeof(fnode));
            fnode.id = ctx->next_node_id++;
            fnode.parent_id = dir->id;
            fnode.backend = ODFS_BACKEND_HFSPLUS;

            /* decode name from key */
            if (key_namelen > 0 && roff + 8 + key_namelen * 2 <= ctx->cat_node_size) {
                hfsp_decode_name(&node[roff + 8], key_namelen,
                                 fnode.name, sizeof(fnode.name));
            }

            /* skip entries with empty or null decoded names */
            if (fnode.name[0] == '\0')
                continue;

            if (rec_type == HFSPLUS_FOLDER_REC && data_len >= 24) {
                fnode.kind = ODFS_NODE_DIR;
                uint32_t cnid = hfsp_be32(&node[data_off + 8]);
                fnode.extent.lba = cnid;
                hfsp_parse_date(hfsp_be32(&node[data_off + 16]), &fnode.mtime);
            } else if (rec_type == HFSPLUS_FILE_REC && data_len >= 88) {
                fnode.kind = ODFS_NODE_FILE;
                /* data fork at record offset 88 (80 bytes) */
                hfsp_fork_t dfork;
                if (data_off + 88 + 80 <= ctx->cat_node_size) {
                    hfsp_parse_fork(&node[data_off + 88], &dfork);
                    fnode.size = dfork.logical_size;
                    fnode.extent.lba = dfork.extents[0].start_block;
                    fnode.extent.length = (uint32_t)dfork.logical_size;
                }
                hfsp_parse_date(hfsp_be32(&node[data_off + 16]), &fnode.mtime);
            }
            fnode.ctime = fnode.mtime;

            entry_index++;
            err = callback(&fnode, cb_ctx);
            if (err != ODFS_OK) {
                free(node);
                if (resume_offset) *resume_offset = (uint32_t)entry_index;
                return err;
            }
        }

        /* next leaf */
        cur_node = hfsp_be32(&node[0]);
        if (found_parent && cur_node != 0) {
            /* peek to see if next node still has our parent */
            err = hfsp_read_node(ctx, cache, cur_node, node);
            if (err != ODFS_OK) break;
            if (node[8] != 0xFF) break;
            uint32_t roff = hfsp_rec_offset(node, ctx->cat_node_size, 0);
            if (roff + 6 < ctx->cat_node_size &&
                hfsp_be32(&node[roff + 2]) != parent_cnid)
                break;
            continue; /* re-process from while loop */
        }
    }

    free(node);
    if (resume_offset) *resume_offset = (uint32_t)entry_index;
    return ODFS_OK;
}

/* ------------------------------------------------------------------ */
/* read                                                                */
/* ------------------------------------------------------------------ */

static odfs_err_t hfsp_read(void *backend_ctx,
                              odfs_cache_t *cache,
                              odfs_log_state_t *log,
                              const odfs_node_t *file,
                              uint64_t offset,
                              void *buf,
                              size_t *len)
{
    hfsplus_context_t *ctx = backend_ctx;
    (void)log;

    if (offset >= file->size) { *len = 0; return ODFS_OK; }

    size_t want = *len;
    if (offset + want > file->size)
        want = (size_t)(file->size - offset);

    /* simple case: first extent only (covers most small files) */
    uint64_t data_start = hfsp_block_to_byte(ctx, file->extent.lba);
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

typedef struct hfsp_lookup_ctx {
    const char   *name;
    odfs_node_t *result;
    int           found;
} hfsp_lookup_ctx_t;

static odfs_err_t hfsp_lookup_cb(const odfs_node_t *entry, void *ctx)
{
    hfsp_lookup_ctx_t *lc = ctx;
    if (strcasecmp(entry->name, lc->name) == 0) {
        *lc->result = *entry;
        lc->found = 1;
        return ODFS_ERR_EOF;
    }
    return ODFS_OK;
}

static odfs_err_t hfsp_lookup(void *backend_ctx,
                                odfs_cache_t *cache,
                                odfs_log_state_t *log,
                                const odfs_node_t *dir,
                                const char *name,
                                odfs_node_t *out)
{
    hfsp_lookup_ctx_t lc = { name, out, 0 };
    odfs_err_t err = hfsp_readdir(backend_ctx, cache, log, dir,
                                    hfsp_lookup_cb, &lc, NULL);
    if (err == ODFS_ERR_EOF && lc.found) return ODFS_OK;
    if (err != ODFS_OK) return err;
    if (lc.found) return ODFS_OK;
    return ODFS_ERR_NOT_FOUND;
}

/* ------------------------------------------------------------------ */
/* get_volume_name                                                     */
/* ------------------------------------------------------------------ */

static odfs_err_t hfsp_get_volume_name(void *backend_ctx,
                                         char *buf, size_t buf_size)
{
    hfsplus_context_t *ctx = backend_ctx;
    size_t len = strlen(ctx->volume_name);
    if (len >= buf_size) len = buf_size - 1;
    memcpy(buf, ctx->volume_name, len);
    buf[len] = '\0';
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
    .unmount         = hfsp_unmount,
    .readdir         = hfsp_readdir,
    .read            = hfsp_read,
    .lookup          = hfsp_lookup,
    .get_volume_name = hfsp_get_volume_name,
};
