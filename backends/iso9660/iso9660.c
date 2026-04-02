/*
 * iso9660.c — ISO 9660 backend implementation
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "iso9660.h"
#include "odfs/cache.h"
#include "odfs/charset.h"
#include "odfs/log.h"
#include "odfs/node.h"
#include "odfs/error.h"

#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

/*
 * Parse a 7-byte directory record date/time (ECMA-119 9.1.5).
 */
static void iso_parse_dir_date(const uint8_t *d, odfs_timestamp_t *ts)
{
    ts->year   = 1900 + d[0];
    ts->month  = d[1];
    ts->day    = d[2];
    ts->hour   = d[3];
    ts->minute = d[4];
    ts->second = d[5];
    ts->tz_offset = (int16_t)((int8_t)d[6]) * 15; /* 15 min intervals */
}

/*
 * Copy a fixed-length string field, trimming trailing spaces and NUL.
 */
static void iso_copy_strfield(const uint8_t *src, size_t src_len,
                              char *dst, size_t dst_size)
{
    size_t len = src_len;
    while (len > 0 && (src[len - 1] == ' ' || src[len - 1] == '\0'))
        len--;
    if (len >= dst_size)
        len = dst_size - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

/*
 * Parse a directory record at the given pointer into an odfs_node_t.
 * Returns the total record length consumed, or 0 on error / end of records.
 */
static int iso_parse_dir_record(const uint8_t *data, size_t avail,
                                uint32_t session_start,
                                uint32_t *next_id,
                                int lowercase,
                                odfs_node_t *node)
{
    uint8_t rec_len;
    uint8_t name_len;
    uint8_t flags;

    if (avail < 1)
        return 0;

    rec_len = data[ISO_DR_LENGTH];
    if (rec_len == 0)
        return 0; /* end of records in this sector */

    if (rec_len < 33 || rec_len > avail)
        return 0; /* malformed */

    name_len = data[ISO_DR_NAME_LEN];
    if (33 + name_len > rec_len)
        return 0; /* malformed */

    memset(node, 0, sizeof(*node));
    node->id = (*next_id)++;
    node->backend = ODFS_BACKEND_ISO9660;

    /* extent and size */
    node->extent.lba    = iso_read_le32(&data[ISO_DR_EXTENT_LBA]) + session_start;
    node->extent.length = iso_read_le32(&data[ISO_DR_DATA_LENGTH]);
    node->size          = node->extent.length;

    /* flags */
    flags = data[ISO_DR_FLAGS];
    node->kind = (flags & ISO_DR_FLAG_DIRECTORY) ? ODFS_NODE_DIR : ODFS_NODE_FILE;

    /* timestamps */
    iso_parse_dir_date(&data[ISO_DR_DATE], &node->mtime);
    node->ctime = node->mtime;

    /* name */
    if (name_len == 1 && data[ISO_DR_NAME] == 0x00) {
        /* "." entry */
        node->name[0] = '.';
        node->name[1] = '\0';
    } else if (name_len == 1 && data[ISO_DR_NAME] == 0x01) {
        /* ".." entry */
        node->name[0] = '.';
        node->name[1] = '.';
        node->name[2] = '\0';
    } else {
        odfs_iso_name_to_display((const char *)&data[ISO_DR_NAME], name_len,
                                  node->name, sizeof(node->name),
                                  lowercase);
    }

    return (int)rec_len;
}

/* ------------------------------------------------------------------ */
/* probe                                                               */
/* ------------------------------------------------------------------ */

static odfs_err_t iso_probe(odfs_cache_t *cache,
                             odfs_log_state_t *log,
                             uint32_t session_start)
{
    const uint8_t *sector;
    odfs_err_t err;

    err = odfs_cache_read(cache, session_start + ISO_VD_START_LBA, &sector);
    if (err != ODFS_OK)
        return err;

    /* check standard identifier "CD001" */
    if (memcmp(&sector[ISO_PVD_ID], ISO_STANDARD_ID, ISO_STANDARD_ID_LEN) != 0) {
        ODFS_DEBUG(log, ODFS_SUB_ISO, "no CD001 signature at LBA %" PRIu32,
                    session_start + ISO_VD_START_LBA);
        return ODFS_ERR_BAD_FORMAT;
    }

    /* check type = PVD (1) */
    if (sector[ISO_PVD_TYPE] != ISO_VD_TYPE_PRIMARY) {
        ODFS_DEBUG(log, ODFS_SUB_ISO, "VD type %u (expected PVD=1)", sector[ISO_PVD_TYPE]);
        return ODFS_ERR_BAD_FORMAT;
    }

    ODFS_INFO(log, ODFS_SUB_ISO, "ISO 9660 PVD found at LBA %" PRIu32,
               session_start + ISO_VD_START_LBA);
    return ODFS_OK;
}

/* ------------------------------------------------------------------ */
/* mount                                                               */
/* ------------------------------------------------------------------ */

static odfs_err_t iso_mount(odfs_cache_t *cache,
                             odfs_log_state_t *log,
                             uint32_t session_start,
                             odfs_node_t *root_out,
                             void **backend_ctx)
{
    const uint8_t *sector;
    iso_context_t *ctx;
    odfs_err_t err;

    ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return ODFS_ERR_NOMEM;

    ctx->session_start = session_start;
    ctx->next_node_id = 1;
    ctx->lowercase = 0; /* preserve original case by default */

    /* read PVD */
    err = odfs_cache_read(cache, session_start + ISO_VD_START_LBA, &sector);
    if (err != ODFS_OK) {
        free(ctx);
        return err;
    }

    /* parse PVD fields */
    iso_copy_strfield(&sector[ISO_PVD_SYSTEM_ID], 32,
                      ctx->pvd.system_id, sizeof(ctx->pvd.system_id));
    iso_copy_strfield(&sector[ISO_PVD_VOLUME_ID], 32,
                      ctx->pvd.volume_id, sizeof(ctx->pvd.volume_id));

    ctx->pvd.volume_space_size = iso_read_le32(&sector[ISO_PVD_VOLUME_SPACE_SIZE]);
    ctx->pvd.logical_block_size = iso_read_le16(&sector[ISO_PVD_LOGICAL_BLK_SIZE]);
    ctx->pvd.path_table_size = iso_read_le32(&sector[ISO_PVD_PATH_TABLE_SIZE]);

    /* root directory record (34 bytes embedded in PVD) */
    memcpy(ctx->pvd.root_dir_record, &sector[ISO_PVD_ROOT_DIR_RECORD], 34);
    ctx->pvd.root_dir_lba  = iso_read_le32(&sector[ISO_PVD_ROOT_DIR_RECORD + ISO_DR_EXTENT_LBA]);
    ctx->pvd.root_dir_size = iso_read_le32(&sector[ISO_PVD_ROOT_DIR_RECORD + ISO_DR_DATA_LENGTH]);

    /* apply session offset to root LBA */
    ctx->pvd.root_dir_lba += session_start;

    ODFS_INFO(log, ODFS_SUB_ISO,
               "volume: \"%s\"  system: \"%s\"  blocks: %" PRIu32 "  blksize: %" PRIu16,
               ctx->pvd.volume_id, ctx->pvd.system_id,
               ctx->pvd.volume_space_size, ctx->pvd.logical_block_size);
    ODFS_INFO(log, ODFS_SUB_ISO,
               "root dir: LBA %" PRIu32 ", size %" PRIu32,
               ctx->pvd.root_dir_lba, ctx->pvd.root_dir_size);

    if (ctx->pvd.logical_block_size != ISO_SECTOR_SIZE) {
        ODFS_WARN(log, ODFS_SUB_ISO,
                   "unusual block size %" PRIu16 " (expected %d)",
                   ctx->pvd.logical_block_size, ISO_SECTOR_SIZE);
    }

    /* build root node */
    memset(root_out, 0, sizeof(*root_out));
    root_out->id         = 0;
    root_out->parent_id  = 0;
    root_out->backend    = ODFS_BACKEND_ISO9660;
    root_out->kind       = ODFS_NODE_DIR;
    root_out->name[0]    = '/';
    root_out->name[1]    = '\0';
    root_out->extent.lba    = ctx->pvd.root_dir_lba;
    root_out->extent.length = ctx->pvd.root_dir_size;
    root_out->size          = ctx->pvd.root_dir_size;

    /* parse timestamps from root dir record */
    iso_parse_dir_date(&ctx->pvd.root_dir_record[ISO_DR_DATE], &root_out->mtime);
    root_out->ctime = root_out->mtime;

    *backend_ctx = ctx;
    return ODFS_OK;
}

/* ------------------------------------------------------------------ */
/* unmount                                                             */
/* ------------------------------------------------------------------ */

static void iso_unmount(void *backend_ctx)
{
    free(backend_ctx);
}

/* ------------------------------------------------------------------ */
/* readdir                                                             */
/* ------------------------------------------------------------------ */

static odfs_err_t iso_readdir(void *backend_ctx,
                               odfs_cache_t *cache,
                               odfs_log_state_t *log,
                               const odfs_node_t *dir,
                               odfs_dir_iter_fn callback,
                               void *cb_ctx)
{
    iso_context_t *ctx = backend_ctx;
    uint32_t dir_lba = dir->extent.lba;
    uint32_t dir_size = dir->extent.length;
    uint32_t offset = 0;
    int lowercase = ctx->lowercase;

    ODFS_TRACE(log, ODFS_SUB_ISO,
                "readdir: LBA %" PRIu32 ", size %" PRIu32, dir_lba, dir_size);

    while (offset < dir_size) {
        uint32_t sector_lba = dir_lba + (offset / ISO_SECTOR_SIZE);
        uint32_t sector_off = offset % ISO_SECTOR_SIZE;
        const uint8_t *sector;
        odfs_err_t err;

        err = odfs_cache_read(cache, sector_lba, &sector);
        if (err != ODFS_OK)
            return err;

        const uint8_t *rec = sector + sector_off;
        size_t avail = ISO_SECTOR_SIZE - sector_off;

        /* zero-length record means skip to next sector */
        if (rec[0] == 0) {
            offset = ((offset / ISO_SECTOR_SIZE) + 1) * ISO_SECTOR_SIZE;
            continue;
        }

        odfs_node_t node;
        int consumed = iso_parse_dir_record(rec, avail, ctx->session_start,
                                            &ctx->next_node_id, lowercase, &node);
        if (consumed == 0) {
            /* skip to next sector on parse failure */
            offset = ((offset / ISO_SECTOR_SIZE) + 1) * ISO_SECTOR_SIZE;
            continue;
        }

        node.parent_id = dir->id;

        /* skip . and .. entries */
        if ((node.name[0] == '.' && node.name[1] == '\0') ||
            (node.name[0] == '.' && node.name[1] == '.' && node.name[2] == '\0')) {
            offset += consumed;
                continue;
        }

        err = callback(&node, cb_ctx);
        if (err != ODFS_OK)
            return err;

        offset += consumed;
    }

    return ODFS_OK;
}

/* ------------------------------------------------------------------ */
/* read                                                                */
/* ------------------------------------------------------------------ */

static odfs_err_t iso_read(void *backend_ctx,
                            odfs_cache_t *cache,
                            odfs_log_state_t *log,
                            const odfs_node_t *file,
                            uint64_t offset,
                            void *buf,
                            size_t *len)
{
    (void)backend_ctx;
    (void)log;
    size_t want = *len;
    size_t done = 0;
    uint8_t *out = buf;

    if (offset >= file->size) {
        *len = 0;
        return ODFS_OK;
    }
    if (offset + want > file->size)
        want = (size_t)(file->size - offset);

    while (done < want) {
        uint64_t file_pos = offset + done;
        uint32_t sector_lba = file->extent.lba + (uint32_t)(file_pos / ISO_SECTOR_SIZE);
        uint32_t sector_off = (uint32_t)(file_pos % ISO_SECTOR_SIZE);
        const uint8_t *sector;
        odfs_err_t err;

        err = odfs_cache_read(cache, sector_lba, &sector);
        if (err != ODFS_OK) {
            *len = done;
            return err;
        }

        size_t chunk = ISO_SECTOR_SIZE - sector_off;
        if (chunk > want - done)
            chunk = want - done;

        memcpy(out + done, sector + sector_off, chunk);
        done += chunk;
    }

    *len = done;
    return ODFS_OK;
}

/* ------------------------------------------------------------------ */
/* lookup                                                              */
/* ------------------------------------------------------------------ */

typedef struct lookup_ctx {
    const char   *name;
    odfs_node_t *result;
    int           found;
} lookup_ctx_t;

static odfs_err_t lookup_cb(const odfs_node_t *entry, void *cb_ctx)
{
    lookup_ctx_t *lctx = cb_ctx;

    /* case-insensitive comparison for ISO names */
    if (strcasecmp(entry->name, lctx->name) == 0) {
        *lctx->result = *entry;
        lctx->found = 1;
        /* we could return an error to stop iteration early,
           but ODFS_OK lets us find duplicates — stop on first match */
        return ODFS_ERR_EOF; /* signal: stop iterating */
    }
    return ODFS_OK;
}

static odfs_err_t iso_lookup(void *backend_ctx,
                              odfs_cache_t *cache,
                              odfs_log_state_t *log,
                              const odfs_node_t *dir,
                              const char *name,
                              odfs_node_t *out)
{
    lookup_ctx_t lctx;
    odfs_err_t err;

    lctx.name = name;
    lctx.result = out;
    lctx.found = 0;

    err = iso_readdir(backend_ctx, cache, log, dir, lookup_cb, &lctx);
    /* ODFS_ERR_EOF from callback means "found, stop early" */
    if (err == ODFS_ERR_EOF && lctx.found)
        return ODFS_OK;
    if (err != ODFS_OK)
        return err;

    if (lctx.found)
        return ODFS_OK;

    return ODFS_ERR_NOT_FOUND;
}

/* ------------------------------------------------------------------ */
/* get_volume_name                                                     */
/* ------------------------------------------------------------------ */

static odfs_err_t iso_get_volume_name(void *backend_ctx,
                                       char *buf, size_t buf_size)
{
    iso_context_t *ctx = backend_ctx;
    size_t len = strlen(ctx->pvd.volume_id);
    if (len >= buf_size)
        len = buf_size - 1;
    memcpy(buf, ctx->pvd.volume_id, len);
    buf[len] = '\0';
    return ODFS_OK;
}

/* ------------------------------------------------------------------ */
/* backend ops table                                                   */
/* ------------------------------------------------------------------ */

const odfs_backend_ops_t iso9660_backend_ops = {
    .name            = "iso9660",
    .backend_type    = ODFS_BACKEND_ISO9660,
    .probe           = iso_probe,
    .mount           = iso_mount,
    .unmount         = iso_unmount,
    .readdir         = iso_readdir,
    .read            = iso_read,
    .lookup          = iso_lookup,
    .get_volume_name = iso_get_volume_name,
};
