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
#include "odfs/alloc.h"
#include "odfs/cache.h"
#include "odfs/charset.h"
#include "odfs/namefix.h"
#include "odfs/log.h"
#include "odfs/error.h"
#include "odfs/string.h"
#include "odfs/timestamp.h"

#include <string.h>
#include <inttypes.h>

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

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

static int hfsp_read_record_count(const uint8_t *node, uint32_t node_size,
                                  uint16_t *nrecs_out)
{
    uint16_t max_recs;
    uint16_t nrecs;

    if (node_size < 12)
        return 0;

    /* Reserve one trailing offset-table slot for r + 1 lookups. */
    max_recs = (uint16_t)((node_size / 2) - 1);
    nrecs = hfsp_be16(&node[10]);
    if (nrecs > max_recs)
        return 0;

    *nrecs_out = nrecs;
    return 1;
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

    ctx = odfs_calloc(1, sizeof(*ctx));
    if (!ctx) return ODFS_ERR_NOMEM;
    ctx->next_node_id = 1;

    err = hfsp_find_vh(cache, session_start, &vh_offset);
    if (err != ODFS_OK) { odfs_free(ctx); return err; }

    /* volume start is 1024 bytes before VH */
    ctx->vol_offset = vh_offset - HFSPLUS_VH_OFFSET;

    /* read Volume Header (512 bytes) */
    uint8_t vh[512];
    {
        uint32_t lba = (uint32_t)(vh_offset / 2048);
        uint32_t off = (uint32_t)(vh_offset % 2048);
        const uint8_t *sector;
        err = odfs_cache_read(cache, lba, &sector);
        if (err != ODFS_OK) { odfs_free(ctx); return err; }
        /* VH might span two sectors if misaligned */
        size_t avail = 2048 - off;
        if (avail >= 512) {
            memcpy(vh, sector + off, 512);
        } else {
            memcpy(vh, sector + off, avail);
            err = odfs_cache_read(cache, lba + 1, &sector);
            if (err != ODFS_OK) { odfs_free(ctx); return err; }
            memcpy(vh + avail, sector, 512 - avail);
        }
    }

    uint16_t sig = hfsp_be16(&vh[0]);
    if (sig != HFSPLUS_SIG && sig != HFSX_SIG) {
        odfs_free(ctx); return ODFS_ERR_BAD_FORMAT;
    }

    ctx->block_size   = hfsp_be32(&vh[40]);
    ctx->total_blocks = hfsp_be32(&vh[44]);

    if (ctx->block_size == 0 || ctx->block_size > 65536) {
        odfs_free(ctx); return ODFS_ERR_BAD_FORMAT;
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
        if (err != ODFS_OK) { odfs_free(ctx); return err; }

        if (hdr_buf[8] != 0x01) { /* node type: header */
            ODFS_ERROR(log, ODFS_SUB_HFSPLUS,
                        "catalog header node type %u", hdr_buf[8]);
            odfs_free(ctx); return ODFS_ERR_BAD_FORMAT;
        }

        /* header record at offset 14 in the node */
        /* B-Tree header record: depth(2) root(4) leafRecords(4)
           firstLeaf(4) lastLeaf(4) nodeSize(2) maxKeyLen(2) ... */
        ctx->cat_root_node  = hfsp_be32(&hdr_buf[14 + 2]);
        ctx->cat_first_leaf = hfsp_be32(&hdr_buf[14 + 2 + 4 + 4]);
        ctx->cat_node_size  = hfsp_be16(&hdr_buf[14 + 2 + 4 + 4 + 4 + 4]);

        if (ctx->cat_node_size < 14 || ctx->cat_node_size > 32768) {
            odfs_free(ctx); return ODFS_ERR_BAD_FORMAT;
        }

        ODFS_INFO(log, ODFS_SUB_HFSPLUS,
                   "catalog: root %" PRIu32 ", node size %" PRIu32,
                   ctx->cat_root_node, ctx->cat_node_size);
    }

    /* try to get volume name from the catalog (root folder thread) */
    {
        uint8_t *node = odfs_malloc(ctx->cat_node_size);
        if (node) {
            /* read root node and scan for folder thread of CNID 2 */
            err = hfsp_read_node(ctx, cache, ctx->cat_first_leaf, node);
            if (err == ODFS_OK && node[8] == 0xFF) { /* leaf */
                uint16_t nrecs;
                if (!hfsp_read_record_count(node, ctx->cat_node_size, &nrecs)) {
                    odfs_free(node);
                    node = NULL;
                }
                if (node) {
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
                            if (data_off + 10 <= ctx->cat_node_size) {
                                uint16_t tnlen = hfsp_be16(&node[data_off + 8]);
                                uint32_t max_name_chars = (ctx->cat_node_size - (data_off + 10)) / 2;
                                if (tnlen > 0 && tnlen <= max_name_chars) {
                                    uint16_t safe_tnlen = tnlen;
                                    hfsp_decode_name(&node[data_off + 10], safe_tnlen,
                                                     ctx->volume_name, sizeof(ctx->volume_name));
                                }
                            }
                        }
                        (void)namelen;
                    }
                }
            }
            odfs_free(node);
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

    odfs_timestamp_from_mac_time(hfsp_be32(&vh[20]),
                                 &root_out->mtime);
    root_out->ctime = root_out->mtime;
    ctx->root = *root_out;

    *backend_ctx = ctx;
    return ODFS_OK;
}

static void hfsp_unmount(void *backend_ctx)
{
    odfs_free(backend_ctx);
}

/* ------------------------------------------------------------------ */
/* catalog walker                                                      */
/* ------------------------------------------------------------------ */

typedef odfs_err_t (*hfsp_cat_cb)(const uint8_t *key, size_t key_len,
                                    const uint8_t *data, size_t data_len,
                                    void *cb_ctx);

static odfs_err_t hfsp_walk_catalog(hfsplus_context_t *ctx,
                                      odfs_cache_t *cache,
                                      uint32_t parent_cnid,
                                      hfsp_cat_cb callback,
                                      void *cb_ctx)
{
    uint8_t *node;
    odfs_err_t err;
    uint32_t cur_node;
    int found_parent = 0;

    node = odfs_malloc(ctx->cat_node_size);
    if (!node)
        return ODFS_ERR_NOMEM;

    cur_node = ctx->cat_root_node;
    for (int depth = 0; depth < 20; depth++) {
        err = hfsp_read_node(ctx, cache, cur_node, node);
        if (err != ODFS_OK) {
            odfs_free(node);
            return err;
        }

        if (node[8] == 0xFF)
            break;
        if (node[8] != 0x00) {
            odfs_free(node);
            return ODFS_ERR_CORRUPT;
        }

        uint16_t nrecs;
        if (!hfsp_read_record_count(node, ctx->cat_node_size, &nrecs)) {
            odfs_free(node);
            return ODFS_ERR_CORRUPT;
        }

        uint32_t child = 0;
        for (uint16_t r = 0; r < nrecs; r++) {
            uint32_t roff = hfsp_rec_offset(node, ctx->cat_node_size, r);
            uint32_t key_parent;
            uint32_t keylen;
            uint32_t ptr_off;
            uint32_t ptr;

            if (roff + 6 >= ctx->cat_node_size)
                break;

            key_parent = hfsp_be32(&node[roff + 2]);
            keylen = hfsp_be16(&node[roff]);
            ptr_off = roff + 2 + keylen;
            if (ptr_off + 4 > ctx->cat_node_size)
                break;

            ptr = hfsp_be32(&node[ptr_off]);
            if (key_parent <= parent_cnid)
                child = ptr;
            else
                break;
        }

        if (child == 0) {
            odfs_free(node);
            return ODFS_ERR_NOT_FOUND;
        }
        cur_node = child;
    }

    while (cur_node != 0) {
        err = hfsp_read_node(ctx, cache, cur_node, node);
        if (err != ODFS_OK) {
            odfs_free(node);
            return err;
        }
        if (node[8] != 0xFF)
            break;

        uint16_t nrecs;
        if (!hfsp_read_record_count(node, ctx->cat_node_size, &nrecs)) {
            odfs_free(node);
            return ODFS_ERR_CORRUPT;
        }

        for (uint16_t r = 0; r < nrecs; r++) {
            uint32_t roff = hfsp_rec_offset(node, ctx->cat_node_size, r);
            uint32_t next_roff =
                hfsp_rec_offset(node, ctx->cat_node_size, r + 1);
            uint32_t keylen;
            uint32_t key_parent;
            uint32_t data_off;
            uint32_t data_len;

            if (roff + 8 >= ctx->cat_node_size)
                continue;
            keylen = hfsp_be16(&node[roff]);
            if (2u + keylen > ctx->cat_node_size - roff)
                continue;
            key_parent = hfsp_be32(&node[roff + 2]);
            if (key_parent < parent_cnid)
                continue;
            if (key_parent > parent_cnid) {
                odfs_free(node);
                return ODFS_OK;
            }
            found_parent = 1;

            data_off = roff + 2 + keylen;
            if (data_off & 1)
                data_off++;
            data_len = (next_roff > data_off) ? next_roff - data_off : 0;
            if (data_off + 2 > ctx->cat_node_size || data_len < 2)
                continue;

            err = callback(&node[roff], 2u + keylen, &node[data_off],
                           data_len, cb_ctx);
            if (err != ODFS_OK) {
                odfs_free(node);
                return err;
            }
        }

        cur_node = hfsp_be32(&node[0]);
        if (found_parent && cur_node != 0) {
            err = hfsp_read_node(ctx, cache, cur_node, node);
            if (err != ODFS_OK) {
                odfs_free(node);
                return err;
            }
            if (node[8] != 0xFF)
                break;
            uint32_t roff = hfsp_rec_offset(node, ctx->cat_node_size, 0);
            if (roff + 6 < ctx->cat_node_size &&
                hfsp_be32(&node[roff + 2]) != parent_cnid)
                break;
            continue;
        }
    }

    odfs_free(node);
    return ODFS_OK;
}

/* ------------------------------------------------------------------ */
/* readdir                                                            */
/* ------------------------------------------------------------------ */

typedef struct hfsp_readdir_ctx {
    hfsplus_context_t *hctx;
    const odfs_node_t *dir;
    odfs_dir_iter_fn callback;
    void *cb_ctx;
    int entry_index;
    int skip_to;
    uint32_t *resume_offset;
    odfs_err_t last_err;
    odfs_namefix_state_t namefix;
} hfsp_readdir_ctx_t;

static odfs_err_t hfsp_readdir_record_cb(const uint8_t *key, size_t key_len,
                                           const uint8_t *data,
                                           size_t data_len,
                                           void *cb_ctx)
{
    hfsp_readdir_ctx_t *rc = cb_ctx;
    uint16_t key_namelen;
    int16_t rec_type;
    odfs_node_t fnode;
    odfs_err_t err;

    if (key_len < 8 || data_len < 2)
        return ODFS_OK;

    rec_type = (int16_t)hfsp_be16(data);
    if (rec_type != HFSPLUS_FOLDER_REC && rec_type != HFSPLUS_FILE_REC)
        return ODFS_OK;

    key_namelen = hfsp_be16(&key[6]);
    if (key_namelen == 0)
        return ODFS_OK;
    if (key_namelen >= 4 && key_len >= 10 &&
        key[8] == 0x00 && key[9] == 0x2E)
        return ODFS_OK;

    memset(&fnode, 0, sizeof(fnode));
    fnode.id = rc->hctx->next_node_id++;
    fnode.parent_id = rc->dir->id;
    fnode.backend = ODFS_BACKEND_HFSPLUS;

    if (key_namelen <= (key_len - 8) / 2)
        hfsp_decode_name(&key[8], key_namelen,
                         fnode.name, sizeof(fnode.name));

    if (fnode.name[0] == '\0')
        return ODFS_OK;

    err = odfs_namefix_apply(&rc->namefix, fnode.name, sizeof(fnode.name));
    if (err != ODFS_OK) {
        rc->last_err = err;
        return err;
    }

    if (rc->entry_index < rc->skip_to) {
        rc->entry_index++;
        return ODFS_OK;
    }

    if (rec_type == HFSPLUS_FOLDER_REC) {
        if (data_len < 24)
            return ODFS_OK;
        fnode.kind = ODFS_NODE_DIR;
        fnode.extent.lba = hfsp_be32(&data[8]);
        odfs_timestamp_from_mac_time(hfsp_be32(&data[16]),
                                     &fnode.mtime);
    } else {
        if (data_len < 88)
            return ODFS_OK;
        fnode.kind = ODFS_NODE_FILE;
        if (data_len >= 168) {
            hfsp_fork_t dfork;
            hfsp_parse_fork(&data[88], &dfork);
            fnode.size = dfork.logical_size;
            fnode.extent.lba = dfork.extents[0].start_block;
            fnode.extent.length = (uint32_t)dfork.logical_size;
        }
        odfs_timestamp_from_mac_time(hfsp_be32(&data[16]),
                                     &fnode.mtime);
    }
    fnode.ctime = fnode.mtime;

    rc->entry_index++;
    err = rc->callback(&fnode, rc->cb_ctx);
    if (err != ODFS_OK) {
        if (rc->resume_offset)
            *rc->resume_offset = (uint32_t)rc->entry_index;
        rc->last_err = err;
        return err;
    }

    return ODFS_OK;
}

static odfs_err_t hfsp_readdir(void *backend_ctx,
                                 odfs_cache_t *cache,
                                 odfs_log_state_t *log,
                                 const odfs_node_t *dir,
                                 odfs_dir_iter_fn callback,
                                 void *cb_ctx,
                                 uint32_t *resume_offset)
{
    hfsplus_context_t *ctx = backend_ctx;
    hfsp_readdir_ctx_t rc;
    odfs_err_t err;

    (void)log;

    rc.hctx = ctx;
    rc.dir = dir;
    rc.callback = callback;
    rc.cb_ctx = cb_ctx;
    rc.entry_index = 0;
    rc.skip_to = (resume_offset && *resume_offset) ? (int)*resume_offset : 0;
    rc.resume_offset = resume_offset;
    rc.last_err = ODFS_OK;
    odfs_namefix_init(&rc.namefix);

    err = hfsp_walk_catalog(ctx, cache, dir->extent.lba,
                            hfsp_readdir_record_cb, &rc);
    odfs_namefix_destroy(&rc.namefix);
    if (rc.last_err != ODFS_OK)
        return rc.last_err;

    if (resume_offset)
        *resume_offset = (uint32_t)rc.entry_index;
    return err;
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
    if (want > file->size - offset)
        want = (size_t)(file->size - offset);

    /* simple case: first extent only (covers most small files) */
    uint64_t data_start = hfsp_block_to_byte(ctx, file->extent.lba);
    *len = want;
    return odfs_cache_read_bytes(cache, 0, data_start + offset, buf, len);
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
    if (odfs_strcasecmp(entry->name, lc->name) == 0) {
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
/* resolve_parent                                                      */
/* ------------------------------------------------------------------ */

typedef struct hfsp_thread_info {
    uint32_t parent_cnid;
    int found;
} hfsp_thread_info_t;

static odfs_err_t hfsp_thread_record_cb(const uint8_t *key, size_t key_len,
                                          const uint8_t *data,
                                          size_t data_len,
                                          void *cb_ctx)
{
    hfsp_thread_info_t *info = cb_ctx;
    int16_t rec_type;

    (void)key;
    (void)key_len;

    if (data_len < 8)
        return ODFS_OK;

    rec_type = (int16_t)hfsp_be16(data);
    if (rec_type != HFSPLUS_FOLDER_THREAD)
        return ODFS_OK;

    info->parent_cnid = hfsp_be32(&data[4]);
    info->found = 1;
    return ODFS_ERR_EOF;
}

static odfs_err_t hfsp_read_thread(hfsplus_context_t *ctx,
                                     odfs_cache_t *cache,
                                     uint32_t cnid,
                                     hfsp_thread_info_t *info)
{
    odfs_err_t err;

    info->parent_cnid = 0;
    info->found = 0;

    err = hfsp_walk_catalog(ctx, cache, cnid, hfsp_thread_record_cb, info);
    if (info->found)
        return ODFS_OK;
    if (err == ODFS_OK || err == ODFS_ERR_EOF)
        return ODFS_ERR_NOT_FOUND;
    return err;
}

typedef struct hfsp_cnid_match {
    uint32_t want_cnid;
    odfs_node_t *out;
    int found;
} hfsp_cnid_match_t;

static odfs_err_t hfsp_cnid_match_cb(const odfs_node_t *entry, void *cb_ctx)
{
    hfsp_cnid_match_t *m = cb_ctx;

    if (entry->kind == ODFS_NODE_DIR && entry->extent.lba == m->want_cnid) {
        *m->out = *entry;
        m->found = 1;
        return ODFS_ERR_EOF;
    }

    return ODFS_OK;
}

static odfs_err_t hfsp_find_child_by_cnid(void *backend_ctx,
                                            odfs_cache_t *cache,
                                            odfs_log_state_t *log,
                                            const odfs_node_t *parent_dir,
                                            uint32_t child_cnid,
                                            odfs_node_t *out)
{
    hfsp_cnid_match_t m;
    odfs_err_t err;

    m.want_cnid = child_cnid;
    m.out = out;
    m.found = 0;

    err = hfsp_readdir(backend_ctx, cache, log, parent_dir,
                       hfsp_cnid_match_cb, &m, NULL);
    if (m.found)
        return ODFS_OK;
    if (err == ODFS_OK || err == ODFS_ERR_EOF)
        return ODFS_ERR_NOT_FOUND;
    return err;
}

static odfs_node_t hfsp_dir_stub(uint32_t cnid)
{
    odfs_node_t n;

    memset(&n, 0, sizeof(n));
    n.backend = ODFS_BACKEND_HFSPLUS;
    n.kind = ODFS_NODE_DIR;
    n.extent.lba = cnid;
    return n;
}

static odfs_err_t hfsp_resolve_parent(void *backend_ctx,
                                        odfs_cache_t *cache,
                                        odfs_log_state_t *log,
                                        const odfs_node_t *dir,
                                        odfs_node_t *parent_out,
                                        odfs_node_t *grandparent_out)
{
    hfsplus_context_t *ctx = backend_ctx;
    hfsp_thread_info_t thread;
    hfsp_thread_info_t parent_thread;
    odfs_node_t grandparent;
    odfs_err_t err;

    if (dir->kind != ODFS_NODE_DIR)
        return ODFS_ERR_NOT_DIR;

    if (dir->extent.lba == HFSPLUS_CNID_ROOT)
        return ODFS_ERR_NOT_FOUND;

    err = hfsp_read_thread(ctx, cache, dir->extent.lba, &thread);
    if (err != ODFS_OK)
        return err;

    if (thread.parent_cnid == HFSPLUS_CNID_ROOT) {
        *parent_out = ctx->root;
        if (grandparent_out)
            *grandparent_out = ctx->root;
        return ODFS_OK;
    }

    err = hfsp_read_thread(ctx, cache, thread.parent_cnid, &parent_thread);
    if (err != ODFS_OK)
        return err;

    if (parent_thread.parent_cnid == HFSPLUS_CNID_ROOT)
        grandparent = ctx->root;
    else
        grandparent = hfsp_dir_stub(parent_thread.parent_cnid);

    err = hfsp_find_child_by_cnid(backend_ctx, cache, log, &grandparent,
                                  thread.parent_cnid, parent_out);
    if (err != ODFS_OK)
        return err;

    if (!grandparent_out)
        return ODFS_OK;

    if (parent_thread.parent_cnid == HFSPLUS_CNID_ROOT) {
        *grandparent_out = ctx->root;
        return ODFS_OK;
    }

    {
        hfsp_thread_info_t gp_thread;
        odfs_node_t great;

        err = hfsp_read_thread(ctx, cache, parent_thread.parent_cnid,
                               &gp_thread);
        if (err != ODFS_OK)
            return err;

        if (gp_thread.parent_cnid == HFSPLUS_CNID_ROOT)
            great = ctx->root;
        else
            great = hfsp_dir_stub(gp_thread.parent_cnid);

        err = hfsp_find_child_by_cnid(backend_ctx, cache, log, &great,
                                      parent_thread.parent_cnid,
                                      grandparent_out);
        if (err != ODFS_OK)
            return err;
    }

    return ODFS_OK;
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
/* get_volume_size                                                     */
/* ------------------------------------------------------------------ */

static uint32_t hfsp_get_volume_size(void *backend_ctx)
{
    hfsplus_context_t *ctx = backend_ctx;
    uint64_t bytes = (uint64_t)ctx->total_blocks * ctx->block_size;
    uint64_t blocks = (bytes + 2047u) / 2048u;

    return blocks > UINT32_MAX ? UINT32_MAX : (uint32_t)blocks;
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
    .resolve_parent  = hfsp_resolve_parent,
    .get_volume_name = hfsp_get_volume_name,
    .get_volume_size = hfsp_get_volume_size,
};
