/*
 * hfs_common.c — code shared by the HFS and HFS+ backends
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * HFS and HFS+ catalogs are the same B-tree with different key and
 * record layouts: the keyLength field is one byte on HFS and two on
 * HFS+, and both sort records by (parentID, name). Each backend
 * supplies the probe, the mount and a parser for its catalog file
 * and folder records; the rest lives here.
 *
 * References: Inside Macintosh: Files, Apple TN1150, Linux fs/hfs/,
 * Linux fs/hfsplus/
 */

#include "hfs/hfs_common.h"
#include "odfs/alloc.h"
#include "odfs/cache.h"
#include "odfs/error.h"
#include "odfs/namefix.h"
#include "odfs/string.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* names                                                               */
/* ------------------------------------------------------------------ */

/* Normalize a few Amiga-problematic bytes in a UTF-8 name. */
void odfs_hfs_fix_name(char *name, size_t len)
{
    for (size_t i = 0; i < len && name[i] != '\0'; i++) {
        unsigned char c = (unsigned char)name[i];

        if (c == ':')
            name[i] = '.';
        else if (c == '/')
            name[i] = '-';
        else if (c < 0x20 || c == 0x7F)
            name[i] = '?';
    }
}

/* ------------------------------------------------------------------ */
/* partition map                                                       */
/* ------------------------------------------------------------------ */

/*
 * Return the 512-byte block at byte_off, which must be 512-aligned,
 * from the block cache. The pointer is valid until the next cache read.
 */
const uint8_t *odfs_hfs_read_block(odfs_cache_t *cache, uint64_t byte_off)
{
    const uint8_t *sector;

    if (byte_off / 2048 > UINT32_MAX ||
        odfs_cache_read(cache, (uint32_t)(byte_off / 2048), &sector) != ODFS_OK)
        return NULL;
    return sector + (uint32_t)(byte_off % 2048);
}

/*
 * Apple Partition Map: entry 1 at 512-byte block 1 is usually the map
 * itself, with the Apple_HFS (or Apple_HFSX) partition in a later
 * entry. pmMapBlkCnt in each entry gives the number of entries.
 */
int odfs_hfs_scan_apm(odfs_cache_t *cache, uint32_t session_start,
                      hfs_part_fn check, void *ctx)
{
    uint64_t base = (uint64_t)session_start * 2048;
    uint32_t count = 1;

    for (uint32_t i = 1; i <= count && i <= 64; i++) {
        const uint8_t *pm = odfs_hfs_read_block(cache, base + i * 512u);
        uint64_t part_byte;

        if (!pm || hfs_be16(pm) != HFS_APM_SIG)
            break;
        if (i == 1)
            count = hfs_be32(&pm[4]);     /* pmMapBlkCnt */
        if (memcmp(&pm[48], "Apple_HFS", 9) != 0)   /* pmPartType */
            continue;

        part_byte = base + (uint64_t)hfs_be32(&pm[8]) * 512;   /* pmPyPartStart */
        if (check(cache, part_byte, ctx))
            return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* B-tree access                                                       */
/* ------------------------------------------------------------------ */

/* read bytes from a B-tree file at a given byte offset within the file */
static odfs_err_t hfs_read_btree(const hfs_volume_t *vol,
                                 const hfs_btree_t *tree,
                                 odfs_cache_t *cache,
                                 uint64_t offset,
                                 uint8_t *buf, size_t len)
{
    size_t done = 0;
    uint64_t ext_offset = 0;

    for (int i = 0; i < 8 && done < len; i++) {
        uint64_t ext_bytes =
            (uint64_t)tree->extents[i].block_count * vol->block_size;
        if (ext_bytes == 0)
            break;

        if (offset < ext_offset + ext_bytes) {
            uint64_t off_in_ext = offset - ext_offset;
            uint64_t phys = vol->alloc_base +
                            (uint64_t)tree->extents[i].start_block *
                            vol->block_size + off_in_ext;

            while (done < len && off_in_ext < ext_bytes) {
                uint32_t lba = (uint32_t)(phys / 2048);
                uint32_t lba_off = (uint32_t)(phys % 2048);
                const uint8_t *sector;
                size_t chunk;

                odfs_err_t err = odfs_cache_read(cache, lba, &sector);
                if (err != ODFS_OK)
                    return err;

                chunk = 2048 - lba_off;
                if (chunk > len - done)
                    chunk = len - done;
                if (chunk > ext_bytes - off_in_ext)
                    chunk = (size_t)(ext_bytes - off_in_ext);

                memcpy(buf + done, sector + lba_off, chunk);
                done += chunk;
                off_in_ext += chunk;
                phys += chunk;
                offset += chunk;
            }
        }
        ext_offset += ext_bytes;
    }

    return (done == len) ? ODFS_OK : ODFS_ERR_RANGE;
}

static odfs_err_t hfs_read_node(const hfs_volume_t *vol,
                                const hfs_btree_t *tree,
                                odfs_cache_t *cache,
                                uint32_t node_num, uint8_t *buf)
{
    return hfs_read_btree(vol, tree, cache,
                          (uint64_t)node_num * tree->node_size,
                          buf, tree->node_size);
}

/* get record offset from the offset table at the end of a node */
static uint32_t hfs_rec_offset(const uint8_t *node, uint32_t node_size,
                               uint32_t rec_idx)
{
    /* offset table is at end of node, growing backward: 2 bytes per entry */
    return hfs_be16(&node[node_size - 2u * (rec_idx + 1u)]);
}

/*
 * Validate the record count and return where the offset table starts;
 * it keeps one slot past the last record for r + 1 lookups. Records
 * must lie between the 14-byte node descriptor and that table.
 */
static int hfs_read_record_count(const uint8_t *node, uint32_t node_size,
                                 uint16_t *nrecs_out, uint32_t *limit_out)
{
    uint16_t nrecs;

    if (node_size < 14)
        return 0;

    nrecs = hfs_be16(&node[10]);
    if (((uint32_t)nrecs + 1u) * 2u + 14u > node_size)
        return 0;

    *nrecs_out = nrecs;
    *limit_out = node_size - 2u * ((uint32_t)nrecs + 1u);
    return 1;
}

static uint32_t hfs_key_length(const hfs_volume_t *vol, const uint8_t *key)
{
    return vol->hfsplus ? 2u + hfs_be16(key) : 1u + key[0];
}

odfs_err_t odfs_hfs_init_btree(const hfs_volume_t *vol, hfs_btree_t *tree,
                               odfs_cache_t *cache)
{
    /* node descriptor and the B-tree header record up to totalNodes */
    uint8_t hdr[40];
    odfs_err_t err;

    err = hfs_read_btree(vol, tree, cache, 0, hdr, sizeof(hdr));
    if (err != ODFS_OK)
        return err;

    if (hdr[8] != HFS_NODE_HEADER)
        return ODFS_ERR_BAD_FORMAT;

    tree->root_node = hfs_be32(&hdr[16]);
    tree->node_size = hfs_be16(&hdr[32]);
    tree->total_nodes = hfs_be32(&hdr[36]);

    if (tree->node_size < 14 || tree->node_size > 32768)
        return ODFS_ERR_BAD_FORMAT;

    return ODFS_OK;
}

/* byte length of an extent, for node->extent.length */
uint32_t odfs_hfs_extent_bytes(const hfs_volume_t *vol, uint32_t blocks)
{
    uint64_t bytes = (uint64_t)blocks * vol->block_size;

    return bytes > UINT32_MAX ? UINT32_MAX : (uint32_t)bytes;
}

/* the walk runs on every readdir and lookup; allocate its buffer once */
odfs_err_t odfs_hfs_alloc_node_buf(hfs_volume_t *vol)
{
    uint32_t size = vol->cat.node_size;

    if (vol->ext.node_size > size)
        size = vol->ext.node_size;
    vol->node_buf = odfs_malloc(size);
    return vol->node_buf ? ODFS_OK : ODFS_ERR_NOMEM;
}

/* ------------------------------------------------------------------ */
/* catalog walk                                                        */
/* ------------------------------------------------------------------ */

/*
 * Call the callback for each catalog record whose key names
 * parent_cnid. The catalog is sorted by (parentID, name), so a
 * directory's records are contiguous: descend to the first leaf that
 * can hold them, then follow the leaf chain until the parent changes.
 */
odfs_err_t odfs_hfs_walk_catalog(const hfs_volume_t *vol,
                                 odfs_cache_t *cache,
                                 uint32_t parent_cnid,
                                 hfs_record_fn callback, void *cb_ctx)
{
    const hfs_btree_t *tree = &vol->cat;
    uint32_t node_size = tree->node_size;
    uint32_t cur_node = tree->root_node;
    uint8_t *node = vol->node_buf;
    uint32_t steps = 0;
    odfs_err_t err;
    int depth;

    /* descend index nodes; the leaf found here stays in the buffer */
    for (depth = 0; depth < 20; depth++) {
        uint16_t nrecs;
        uint32_t limit;
        uint32_t child = 0;

        err = hfs_read_node(vol, tree, cache, cur_node, node);
        if (err != ODFS_OK)
            return err;

        if (node[8] == HFS_NODE_LEAF)
            break;
        if (node[8] != HFS_NODE_INDEX ||
            !hfs_read_record_count(node, node_size, &nrecs, &limit))
            return ODFS_ERR_CORRUPT;

        for (uint16_t r = 0; r < nrecs; r++) {
            uint32_t off = hfs_rec_offset(node, node_size, r);
            uint32_t key_len;
            uint32_t key_parent;
            uint32_t ptr_off;
            uint32_t ptr;

            if (off < 14 || off + 6 > limit)
                break;
            key_len = hfs_key_length(vol, &node[off]);
            if (key_len < 6 || key_len > limit - off)
                break;

            /* the child pointer follows the key, padded to even */
            ptr_off = off + key_len;
            if (ptr_off & 1)
                ptr_off++;
            if (ptr_off + 4 > limit)
                break;

            key_parent = hfs_be32(&node[off + 2]);
            ptr = hfs_be32(&node[ptr_off]);

            /* a directory's records can start in the child before the
             * first key naming it */
            if (key_parent < parent_cnid || child == 0)
                child = ptr;
            if (key_parent >= parent_cnid)
                break;
        }

        if (child == 0)
            return ODFS_ERR_NOT_FOUND;
        cur_node = child;
    }
    if (depth == 20)
        return ODFS_ERR_LOOP;

    /* scan leaf nodes for records of parent_cnid */
    for (;;) {
        uint16_t nrecs;
        uint32_t limit;
        uint32_t prev_node;

        if (!hfs_read_record_count(node, node_size, &nrecs, &limit))
            return ODFS_ERR_CORRUPT;

        for (uint16_t r = 0; r < nrecs; r++) {
            uint32_t off = hfs_rec_offset(node, node_size, r);
            uint32_t next_off = hfs_rec_offset(node, node_size, r + 1u);
            uint32_t key_len;
            uint32_t key_parent;
            uint32_t data_off;

            if (off < 14 || off + 6 > limit)
                continue;
            key_len = hfs_key_length(vol, &node[off]);
            if (key_len < 6 || key_len > limit - off)
                continue;

            key_parent = hfs_be32(&node[off + 2]);
            if (key_parent < parent_cnid)
                continue;
            if (key_parent > parent_cnid)
                return ODFS_OK; /* past our directory — done */

            /* record data follows the key (padded to even boundary) */
            data_off = off + key_len;
            if (data_off & 1)
                data_off++;
            if (next_off > limit)
                next_off = limit;
            if (next_off <= data_off)
                continue;

            err = callback(&node[off], key_len, &node[data_off],
                           next_off - data_off, cb_ctx);
            if (err != ODFS_OK)
                return err;
        }

        /* follow forward link to next leaf */
        prev_node = cur_node;
        cur_node = hfs_be32(&node[0]);
        if (cur_node == 0)
            return ODFS_OK;
        if (++steps >= tree->total_nodes)
            return ODFS_ERR_LOOP;

        err = hfs_read_node(vol, tree, cache, cur_node, node);
        if (err != ODFS_OK || node[8] != HFS_NODE_LEAF)
            return err;
        /* the leaf chain is doubly linked; a self-link or a jump out of
         * the chain shows up as a back link that does not match */
        if (hfs_be32(&node[4]) != prev_node)
            return ODFS_ERR_CORRUPT;
    }
}

/* ------------------------------------------------------------------ */
/* readdir                                                             */
/* ------------------------------------------------------------------ */

typedef struct hfs_readdir_ctx {
    const hfs_volume_t *vol;
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
    odfs_node_t node;
    odfs_err_t err;

    if (!rc->vol->parse_entry(rc->vol, rc->dir, key, key_len,
                              data, data_len, &node))
        return ODFS_OK;

    err = odfs_namefix_apply(&rc->namefix, node.name, sizeof(node.name));
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

odfs_err_t odfs_hfs_readdir(void *backend_ctx,
                            odfs_cache_t *cache,
                            odfs_log_state_t *log,
                            const odfs_node_t *dir,
                            odfs_dir_iter_fn callback,
                            void *cb_ctx,
                            uint32_t *resume_offset)
{
    const hfs_volume_t *vol = backend_ctx;
    hfs_readdir_ctx_t rc;
    odfs_err_t err;

    (void)log;

    rc.vol = vol;
    rc.dir = dir;
    rc.callback = callback;
    rc.cb_ctx = cb_ctx;
    rc.entry_index = 0;
    rc.skip_to = (resume_offset && *resume_offset) ? *resume_offset : 0;
    rc.resume_offset = resume_offset;
    rc.last_err = ODFS_OK;
    odfs_namefix_init(&rc.namefix);

    /* dir->extent.lba stores the directory's CNID */
    err = odfs_hfs_walk_catalog(vol, cache, dir->extent.lba,
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

/*
 * A file's data fork lists its first extents in the catalog record,
 * three on HFS and eight on HFS+; further extents sit in records of
 * the extents overflow B-tree keyed by (fileID, forkType, startBlock).
 */

typedef struct hfs_inline_ext {
    uint32_t file_id;
    uint8_t hfsplus;
    uint8_t found;
    hfs_extent_t *exts;
} hfs_inline_ext_t;

static odfs_err_t hfs_inline_ext_cb(const uint8_t *key, size_t key_len,
                                    const uint8_t *data, size_t data_len,
                                    void *cb_ctx)
{
    hfs_inline_ext_t *ie = cb_ctx;

    (void)key;
    (void)key_len;

    if (ie->hfsplus) {
        /* fileID at 8, data fork extents at 104 */
        if (data_len < 168 || hfs_be16(data) != HFS_CAT_FILE ||
            hfs_be32(&data[8]) != ie->file_id)
            return ODFS_OK;
        for (int i = 0; i < 8; i++) {
            ie->exts[i].start_block = hfs_be32(&data[104 + i * 8]);
            ie->exts[i].block_count = hfs_be32(&data[108 + i * 8]);
        }
    } else {
        /* filFlNum at 20, filExtRec at 74 */
        if (data_len < 86 || data[0] != HFS_CAT_FILE ||
            hfs_be32(&data[20]) != ie->file_id)
            return ODFS_OK;
        for (int i = 0; i < 3; i++) {
            ie->exts[i].start_block = hfs_be16(&data[74 + i * 4]);
            ie->exts[i].block_count = hfs_be16(&data[76 + i * 4]);
        }
    }

    ie->found = 1;
    return ODFS_ERR_EOF;
}

/* the inline extents of file, from its record in the parent directory */
static odfs_err_t hfs_read_inline_ext(const hfs_volume_t *vol,
                                      odfs_cache_t *cache,
                                      const odfs_node_t *file,
                                      hfs_extent_t exts[8])
{
    hfs_inline_ext_t ie;
    odfs_err_t err;

    memset(exts, 0, 8 * sizeof(*exts));
    ie.file_id = file->id;
    ie.hfsplus = vol->hfsplus;
    ie.found = 0;
    ie.exts = exts;

    err = odfs_hfs_walk_catalog(vol, cache,
                                file->parent_id ? file->parent_id
                                                : HFS_CNID_ROOT_DIR,
                                hfs_inline_ext_cb, &ie);
    if (ie.found)
        return ODFS_OK;
    if (err == ODFS_OK || err == ODFS_ERR_EOF)
        return ODFS_ERR_NOT_FOUND;
    return err;
}

/*
 * Find the extents overflow record of file_id's data fork that holds
 * fork byte offset: the last record whose key is not above
 * (file_id, data fork, offset). Returns its extents and the fork
 * offset of the first one.
 */
static odfs_err_t hfs_find_overflow(const hfs_volume_t *vol,
                                    odfs_cache_t *cache,
                                    uint32_t file_id, uint64_t offset,
                                    hfs_extent_t exts[8], uint64_t *base)
{
    const hfs_btree_t *tree = &vol->ext;
    uint32_t node_size = tree->node_size;
    uint32_t cur_node = tree->root_node;
    uint8_t *node = vol->node_buf;
    /* HFS key: keyLength(1) forkType(1) fileID(4) startBlock(2);
       HFS+: keyLength(2) forkType(1) pad(1) fileID(4) startBlock(4) */
    uint32_t min_key = vol->hfsplus ? 12 : 8;
    uint32_t fork_off = vol->hfsplus ? 2 : 1;
    uint32_t id_off = vol->hfsplus ? 4 : 2;

    if (node_size == 0)
        return ODFS_ERR_CORRUPT;

    for (int depth = 0; depth < 20; depth++) {
        uint16_t nrecs;
        uint32_t limit;
        uint32_t best = 0;
        uint32_t best_start = 0;
        uint32_t ptr_off;
        odfs_err_t err;

        err = hfs_read_node(vol, tree, cache, cur_node, node);
        if (err != ODFS_OK)
            return err;
        if ((node[8] != HFS_NODE_LEAF && node[8] != HFS_NODE_INDEX) ||
            !hfs_read_record_count(node, node_size, &nrecs, &limit))
            return ODFS_ERR_CORRUPT;

        for (uint16_t r = 0; r < nrecs; r++) {
            uint32_t off = hfs_rec_offset(node, node_size, r);
            uint32_t id;
            uint32_t start;

            if (off < 14 || off + min_key > limit)
                break;
            id = hfs_be32(&node[off + id_off]);
            start = vol->hfsplus ? hfs_be32(&node[off + 8])
                                 : hfs_be16(&node[off + 6]);
            if (id > file_id ||
                (id == file_id &&
                 (node[off + fork_off] != 0 ||
                  (uint64_t)start * vol->block_size > offset)))
                break;
            best = off;
            best_start = start;
        }
        if (best == 0)
            return ODFS_ERR_CORRUPT;

        ptr_off = best + hfs_key_length(vol, &node[best]);
        if (ptr_off & 1)
            ptr_off++;

        if (node[8] == HFS_NODE_INDEX) {
            if (ptr_off < best + min_key || ptr_off + 4 > limit)
                return ODFS_ERR_CORRUPT;
            cur_node = hfs_be32(&node[ptr_off]);
            continue;
        }

        if (hfs_be32(&node[best + id_off]) != file_id ||
            node[best + fork_off] != 0 || ptr_off < best + min_key ||
            ptr_off + (vol->hfsplus ? 64u : 12u) > limit)
            return ODFS_ERR_CORRUPT;

        memset(exts, 0, 8 * sizeof(*exts));
        for (uint32_t i = 0; i < (vol->hfsplus ? 8u : 3u); i++) {
            const uint8_t *e = &node[ptr_off + i * (vol->hfsplus ? 8u : 4u)];

            exts[i].start_block = vol->hfsplus ? hfs_be32(e) : hfs_be16(e);
            exts[i].block_count = vol->hfsplus ? hfs_be32(e + 4)
                                               : hfs_be16(e + 2);
        }
        *base = (uint64_t)best_start * vol->block_size;
        return ODFS_OK;
    }
    return ODFS_ERR_LOOP;
}

/*
 * Copy what extents exts, which start at fork offset base, hold from
 * fork offset *offset on, up to want bytes in total.
 */
static odfs_err_t hfs_read_extents(const hfs_volume_t *vol,
                                   odfs_cache_t *cache,
                                   const hfs_extent_t exts[8],
                                   uint64_t base, uint64_t *offset,
                                   uint8_t *buf, size_t want, size_t *done)
{
    for (int i = 0; i < 8 && *done < want; i++) {
        uint64_t ext_bytes =
            (uint64_t)exts[i].block_count * vol->block_size;
        size_t chunk;
        size_t got;
        odfs_err_t err;

        if (ext_bytes == 0)
            break;
        if (*offset >= base + ext_bytes) {
            base += ext_bytes;
            continue;
        }

        chunk = want - *done;
        if (chunk > base + ext_bytes - *offset)
            chunk = (size_t)(base + ext_bytes - *offset);
        got = chunk;
        err = odfs_cache_read_bytes(cache, 0,
                                    vol->alloc_base +
                                    (uint64_t)exts[i].start_block *
                                    vol->block_size + (*offset - base),
                                    buf + *done, &got);
        *done += got;
        *offset += got;
        if (err != ODFS_OK)
            return err;
        if (got != chunk)
            return ODFS_ERR_IO;
        base += ext_bytes;
    }
    return ODFS_OK;
}

odfs_err_t odfs_hfs_read(void *backend_ctx,
                         odfs_cache_t *cache,
                         odfs_log_state_t *log,
                         const odfs_node_t *file,
                         uint64_t offset,
                         void *buf,
                         size_t *len)
{
    hfs_volume_t *vol = backend_ctx;
    hfs_extent_map_t *map = &vol->map;
    size_t want;
    size_t done = 0;
    odfs_err_t err;

    (void)log;

    if (offset >= file->size) {
        *len = 0;
        return ODFS_OK;
    }

    want = *len;
    if (want > file->size - offset)
        want = (size_t)(file->size - offset);

    /* file->extent holds the first extent: start block and byte length */
    if (offset + want <= file->extent.length) {
        *len = want;
        return odfs_cache_read_bytes(cache, 0,
                                     vol->alloc_base +
                                     (uint64_t)file->extent.lba *
                                     vol->block_size + offset,
                                     buf, len);
    }

    /*
     * Keep the extent map of the last file read, so that reading a
     * fragmented file in chunks walks its directory and the overflow
     * tree once per record instead of once per chunk. The volume is
     * read-only and CNIDs are unique, so the map stays valid.
     */
    err = ODFS_OK;
    if (file->id == 0 || map->file_id != file->id) {
        map->file_id = 0;
        map->ov_ext[0].block_count = 0;
        err = hfs_read_inline_ext(vol, cache, file, map->inline_ext);
        if (err == ODFS_OK)
            map->file_id = file->id;
    }
    if (err == ODFS_OK)
        err = hfs_read_extents(vol, cache, map->inline_ext, 0, &offset,
                               buf, want, &done);

    while (err == ODFS_OK && done < want) {
        size_t before = done;

        if (map->ov_ext[0].block_count != 0 && map->ov_base <= offset)
            err = hfs_read_extents(vol, cache, map->ov_ext, map->ov_base,
                                   &offset, buf, want, &done);
        if (err != ODFS_OK || done != before)
            continue;

        err = hfs_find_overflow(vol, cache, file->id, offset,
                                map->ov_ext, &map->ov_base);
        if (err == ODFS_OK)
            err = hfs_read_extents(vol, cache, map->ov_ext, map->ov_base,
                                   &offset, buf, want, &done);
        if (err == ODFS_OK && done == before)
            err = ODFS_ERR_CORRUPT;   /* a hole in the extent map */
    }

    *len = done;
    return err;
}

/* ------------------------------------------------------------------ */
/* lookup                                                              */
/* ------------------------------------------------------------------ */

typedef struct hfs_lookup_ctx {
    const char *name;
    odfs_node_t *result;
    int found;
} hfs_lookup_ctx_t;

static odfs_err_t hfs_lookup_cb(const odfs_node_t *entry, void *ctx)
{
    hfs_lookup_ctx_t *lc = ctx;

    if (odfs_strcasecmp(entry->name, lc->name) == 0) {
        *lc->result = *entry;
        lc->found = 1;
        return ODFS_ERR_EOF;
    }
    return ODFS_OK;
}

odfs_err_t odfs_hfs_lookup(void *backend_ctx,
                           odfs_cache_t *cache,
                           odfs_log_state_t *log,
                           const odfs_node_t *dir,
                           const char *name,
                           odfs_node_t *out)
{
    hfs_lookup_ctx_t lc = { name, out, 0 };
    odfs_err_t err = odfs_hfs_readdir(backend_ctx, cache, log, dir,
                                      hfs_lookup_cb, &lc, NULL);

    if (err == ODFS_ERR_EOF && lc.found)
        return ODFS_OK;
    if (err != ODFS_OK)
        return err;
    if (lc.found)
        return ODFS_OK;
    return ODFS_ERR_NOT_FOUND;
}

/* ------------------------------------------------------------------ */
/* resolve_parent                                                      */
/* ------------------------------------------------------------------ */

typedef struct hfs_thread_info {
    uint32_t parent_cnid;
    uint8_t hfsplus;
    uint8_t found;
} hfs_thread_info_t;

static odfs_err_t hfs_thread_cb(const uint8_t *key, size_t key_len,
                                const uint8_t *data, size_t data_len,
                                void *cb_ctx)
{
    hfs_thread_info_t *info = cb_ctx;

    (void)key;
    (void)key_len;

    if (info->hfsplus) {
        /* type(2) reserved(2) parentID(4) nodeName */
        if (data_len < 8 || hfs_be16(data) != HFS_CAT_DTHREAD)
            return ODFS_OK;
        info->parent_cnid = hfs_be32(&data[4]);
    } else {
        /* type(1) reserved(1) reserved(8) parentID(4) nodeName */
        if (data_len < 14 || data[0] != HFS_CAT_DTHREAD)
            return ODFS_OK;
        info->parent_cnid = hfs_be32(&data[10]);
    }

    info->found = 1;
    return ODFS_ERR_EOF;
}

static odfs_err_t hfs_read_thread(const hfs_volume_t *vol,
                                  odfs_cache_t *cache,
                                  uint32_t cnid,
                                  uint32_t *parent_cnid_out)
{
    hfs_thread_info_t info;
    odfs_err_t err;

    info.parent_cnid = 0;
    info.hfsplus = vol->hfsplus;
    info.found = 0;

    err = odfs_hfs_walk_catalog(vol, cache, cnid, hfs_thread_cb, &info);
    if (info.found) {
        *parent_cnid_out = info.parent_cnid;
        return ODFS_OK;
    }
    if (err == ODFS_OK || err == ODFS_ERR_EOF)
        return ODFS_ERR_NOT_FOUND;
    return err;
}

typedef struct hfs_cnid_match {
    uint32_t want_cnid;
    odfs_node_t *out;
    int found;
} hfs_cnid_match_t;

static odfs_err_t hfs_cnid_match_cb(const odfs_node_t *entry, void *cb_ctx)
{
    hfs_cnid_match_t *m = cb_ctx;

    if (entry->kind == ODFS_NODE_DIR && entry->extent.lba == m->want_cnid) {
        *m->out = *entry;
        m->found = 1;
        return ODFS_ERR_EOF;
    }

    return ODFS_OK;
}

static odfs_err_t hfs_find_child_by_cnid(void *backend_ctx,
                                         odfs_cache_t *cache,
                                         odfs_log_state_t *log,
                                         const odfs_node_t *parent_dir,
                                         uint32_t child_cnid,
                                         odfs_node_t *out)
{
    hfs_cnid_match_t m = { child_cnid, out, 0 };
    odfs_err_t err = odfs_hfs_readdir(backend_ctx, cache, log, parent_dir,
                                      hfs_cnid_match_cb, &m, NULL);

    if (m.found)
        return ODFS_OK;
    if (err == ODFS_OK || err == ODFS_ERR_EOF)
        return ODFS_ERR_NOT_FOUND;
    return err;
}

static odfs_node_t hfs_dir_stub(const hfs_volume_t *vol, uint32_t cnid)
{
    odfs_node_t n;

    if (cnid == HFS_CNID_ROOT_DIR)
        return vol->root;

    memset(&n, 0, sizeof(n));
    n.id = cnid;
    n.backend = vol->root.backend;
    n.kind = ODFS_NODE_DIR;
    n.extent.lba = cnid;
    return n;
}

odfs_err_t odfs_hfs_resolve_parent(void *backend_ctx,
                                   odfs_cache_t *cache,
                                   odfs_log_state_t *log,
                                   const odfs_node_t *dir,
                                   odfs_node_t *parent_out,
                                   odfs_node_t *grandparent_out)
{
    const hfs_volume_t *vol = backend_ctx;
    uint32_t parent_cnid;
    uint32_t gp_cnid;
    uint32_t great_cnid;
    odfs_node_t stub;
    odfs_err_t err;

    if (dir->kind != ODFS_NODE_DIR)
        return ODFS_ERR_NOT_DIR;

    if (dir->extent.lba == HFS_CNID_ROOT_DIR)
        return ODFS_ERR_NOT_FOUND;

    err = hfs_read_thread(vol, cache, dir->extent.lba, &parent_cnid);
    if (err != ODFS_OK)
        return err;

    if (parent_cnid == HFS_CNID_ROOT_DIR) {
        *parent_out = vol->root;
        if (grandparent_out)
            *grandparent_out = vol->root;
        return ODFS_OK;
    }

    err = hfs_read_thread(vol, cache, parent_cnid, &gp_cnid);
    if (err != ODFS_OK)
        return err;

    stub = hfs_dir_stub(vol, gp_cnid);
    err = hfs_find_child_by_cnid(backend_ctx, cache, log, &stub,
                                 parent_cnid, parent_out);
    if (err != ODFS_OK || !grandparent_out)
        return err;

    if (gp_cnid == HFS_CNID_ROOT_DIR) {
        *grandparent_out = vol->root;
        return ODFS_OK;
    }

    err = hfs_read_thread(vol, cache, gp_cnid, &great_cnid);
    if (err != ODFS_OK)
        return err;

    stub = hfs_dir_stub(vol, great_cnid);
    return hfs_find_child_by_cnid(backend_ctx, cache, log, &stub,
                                  gp_cnid, grandparent_out);
}

/* ------------------------------------------------------------------ */
/* volume metadata                                                     */
/* ------------------------------------------------------------------ */

odfs_err_t odfs_hfs_get_volume_name(void *backend_ctx,
                                    char *buf, size_t buf_size)
{
    const hfs_volume_t *vol = backend_ctx;
    size_t len = strlen(vol->volume_name);

    if (len >= buf_size)
        len = buf_size - 1;
    memcpy(buf, vol->volume_name, len);
    buf[len] = '\0';
    return ODFS_OK;
}

uint32_t odfs_hfs_get_volume_size(void *backend_ctx)
{
    const hfs_volume_t *vol = backend_ctx;
    uint64_t bytes = (uint64_t)vol->total_blocks * vol->block_size;
    uint64_t blocks = (bytes + 2047u) / 2048u;

    return blocks > UINT32_MAX ? UINT32_MAX : (uint32_t)blocks;
}

void odfs_hfs_unmount(void *backend_ctx)
{
    hfs_volume_t *vol = backend_ctx;

    odfs_free(vol->node_buf);
    odfs_free(vol);
}
