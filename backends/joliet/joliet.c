/*
 * joliet.c — Joliet backend (SVD with UCS-2 filenames)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Joliet uses a Supplementary Volume Descriptor (type 2) with UCS-2
 * encoded filenames. It has its own directory tree, separate from the
 * ISO 9660 primary directory tree.
 */

#include "joliet.h"
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
 * Parse a 7-byte directory record date/time.
 */
static void joliet_parse_dir_date(const uint8_t *d, odfs_timestamp_t *ts)
{
    ts->year      = 1900 + d[0];
    ts->month     = d[1];
    ts->day       = d[2];
    ts->hour      = d[3];
    ts->minute    = d[4];
    ts->second    = d[5];
    ts->tz_offset = (int16_t)((int8_t)d[6]) * 15;
}

/*
 * Parse a Joliet directory record. Names are UCS-2 BE, converted to UTF-8.
 * Returns record length consumed, or 0 on error.
 */
static int joliet_parse_dir_record(const uint8_t *data, size_t avail,
                                    uint32_t session_start,
                                    uint32_t *next_id,
                                    odfs_node_t *node)
{
    uint8_t rec_len, name_len, flags;

    if (avail < 1)
        return 0;

    rec_len = data[ISO_DR_LENGTH];
    if (rec_len == 0)
        return 0;

    if (rec_len < 33 || rec_len > avail)
        return 0;

    name_len = data[ISO_DR_NAME_LEN];
    if (33 + name_len > rec_len)
        return 0;

    memset(node, 0, sizeof(*node));
    node->id = (*next_id)++;
    node->backend = ODFS_BACKEND_JOLIET;

    node->extent.lba    = iso_read_le32(&data[ISO_DR_EXTENT_LBA]) + session_start;
    node->extent.length = iso_read_le32(&data[ISO_DR_DATA_LENGTH]);
    node->size          = node->extent.length;

    flags = data[ISO_DR_FLAGS];
    node->kind = (flags & ISO_DR_FLAG_DIRECTORY) ? ODFS_NODE_DIR : ODFS_NODE_FILE;

    joliet_parse_dir_date(&data[ISO_DR_DATE], &node->mtime);
    node->ctime = node->mtime;

    /* name: UCS-2 BE → UTF-8 */
    if (name_len == 1 && data[ISO_DR_NAME] == 0x00) {
        node->name[0] = '.';
        node->name[1] = '\0';
    } else if (name_len == 1 && data[ISO_DR_NAME] == 0x01) {
        node->name[0] = '.';
        node->name[1] = '.';
        node->name[2] = '\0';
    } else {
        size_t out_len;
        odfs_ucs2be_to_utf8(&data[ISO_DR_NAME], name_len,
                              node->name, sizeof(node->name), &out_len);
        /* strip ";1" version suffix if present */
        if (out_len >= 2 && node->name[out_len - 2] == ';')
            node->name[out_len - 2] = '\0';
    }

    return (int)rec_len;
}

/* ------------------------------------------------------------------ */
/* probe — look for Joliet SVD                                         */
/* ------------------------------------------------------------------ */

/*
 * Scan volume descriptors for a Supplementary VD with Joliet escape
 * sequences in the escape field (bytes 88-90).
 */
static odfs_err_t joliet_probe(odfs_cache_t *cache,
                                odfs_log_state_t *log,
                                uint32_t session_start)
{
    uint32_t lba;

    for (lba = session_start + ISO_VD_START_LBA; ; lba++) {
        const uint8_t *sector;
        odfs_err_t err = odfs_cache_read(cache, lba, &sector);
        if (err != ODFS_OK)
            return err;

        uint8_t vd_type = sector[0];

        /* check CD001 signature */
        if (memcmp(&sector[1], ISO_STANDARD_ID, ISO_STANDARD_ID_LEN) != 0)
            return ODFS_ERR_BAD_FORMAT;

        if (vd_type == ISO_VD_TYPE_TERM)
            break; /* no Joliet SVD found */

        if (vd_type == ISO_VD_TYPE_SUPPL) {
            /* check for Joliet escape sequences at offset 88 */
            const uint8_t *esc = &sector[88];
            if ((esc[0] == '%' && esc[1] == '/' &&
                 (esc[2] == '@' || esc[2] == 'C' || esc[2] == 'E'))) {
                ODFS_INFO(log, ODFS_SUB_JOLIET,
                           "Joliet SVD found at LBA %" PRIu32
                           " (level %c)", lba, esc[2]);
                return ODFS_OK;
            }
        }

        /* safety: don't scan forever */
        if (lba > session_start + ISO_VD_START_LBA + 32)
            break;
    }

    return ODFS_ERR_BAD_FORMAT;
}

/* ------------------------------------------------------------------ */
/* mount                                                               */
/* ------------------------------------------------------------------ */

static odfs_err_t joliet_mount(odfs_cache_t *cache,
                                odfs_log_state_t *log,
                                uint32_t session_start,
                                odfs_node_t *root_out,
                                void **backend_ctx)
{
    joliet_context_t *ctx;
    uint32_t lba;
    const uint8_t *svd_sector = NULL;

    ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return ODFS_ERR_NOMEM;

    ctx->session_start = session_start;
    ctx->next_node_id = 1;

    /* find the Joliet SVD */
    for (lba = session_start + ISO_VD_START_LBA; ; lba++) {
        const uint8_t *sector;
        odfs_err_t err = odfs_cache_read(cache, lba, &sector);
        if (err != ODFS_OK) { free(ctx); return err; }

        if (sector[0] == ISO_VD_TYPE_TERM)
            break;

        if (sector[0] == ISO_VD_TYPE_SUPPL) {
            const uint8_t *esc = &sector[88];
            if (esc[0] == '%' && esc[1] == '/' &&
                (esc[2] == '@' || esc[2] == 'C' || esc[2] == 'E')) {
                svd_sector = sector;
                break;
            }
        }
        if (lba > session_start + ISO_VD_START_LBA + 32)
            break;
    }

    if (!svd_sector) {
        free(ctx);
        return ODFS_ERR_BAD_FORMAT;
    }

    /* parse SVD — same layout as PVD but volume ID is UCS-2 */
    {
        size_t vname_len;
        odfs_ucs2be_to_utf8(&svd_sector[ISO_PVD_VOLUME_ID], 32,
                              ctx->svd.volume_id, sizeof(ctx->svd.volume_id),
                              &vname_len);
        /* trim trailing spaces */
        while (vname_len > 0 && ctx->svd.volume_id[vname_len - 1] == ' ')
            ctx->svd.volume_id[--vname_len] = '\0';
    }

    iso_copy_strfield(&svd_sector[ISO_PVD_SYSTEM_ID], 32,
                      ctx->svd.system_id, sizeof(ctx->svd.system_id));

    ctx->svd.volume_space_size = iso_read_le32(&svd_sector[ISO_PVD_VOLUME_SPACE_SIZE]);
    ctx->svd.logical_block_size = iso_read_le16(&svd_sector[ISO_PVD_LOGICAL_BLK_SIZE]);

    memcpy(ctx->svd.root_dir_record, &svd_sector[ISO_PVD_ROOT_DIR_RECORD], 34);
    ctx->svd.root_dir_lba  = iso_read_le32(&svd_sector[ISO_PVD_ROOT_DIR_RECORD + ISO_DR_EXTENT_LBA]);
    ctx->svd.root_dir_size = iso_read_le32(&svd_sector[ISO_PVD_ROOT_DIR_RECORD + ISO_DR_DATA_LENGTH]);
    ctx->svd.root_dir_lba += session_start;

    ODFS_INFO(log, ODFS_SUB_JOLIET,
               "volume: \"%s\"  root LBA: %" PRIu32,
               ctx->svd.volume_id, ctx->svd.root_dir_lba);

    /* build root node */
    memset(root_out, 0, sizeof(*root_out));
    root_out->id            = 0;
    root_out->parent_id     = 0;
    root_out->backend       = ODFS_BACKEND_JOLIET;
    root_out->kind          = ODFS_NODE_DIR;
    root_out->name[0]       = '/';
    root_out->name[1]       = '\0';
    root_out->extent.lba    = ctx->svd.root_dir_lba;
    root_out->extent.length = ctx->svd.root_dir_size;
    root_out->size          = ctx->svd.root_dir_size;

    joliet_parse_dir_date(&ctx->svd.root_dir_record[ISO_DR_DATE], &root_out->mtime);
    root_out->ctime = root_out->mtime;

    *backend_ctx = ctx;
    return ODFS_OK;
}

/* ------------------------------------------------------------------ */
/* unmount                                                             */
/* ------------------------------------------------------------------ */

static void joliet_unmount(void *backend_ctx)
{
    free(backend_ctx);
}

/* ------------------------------------------------------------------ */
/* readdir                                                             */
/* ------------------------------------------------------------------ */

static odfs_err_t joliet_readdir(void *backend_ctx,
                                  odfs_cache_t *cache,
                                  odfs_log_state_t *log,
                                  const odfs_node_t *dir,
                                  odfs_dir_iter_fn callback,
                                  void *cb_ctx,
                                  uint32_t *resume_offset)
{
    joliet_context_t *ctx = backend_ctx;
    uint32_t dir_lba = dir->extent.lba;
    uint32_t dir_size = dir->extent.length;
    uint32_t offset = (resume_offset && *resume_offset) ? *resume_offset : 0;

    (void)log;

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

        if (rec[0] == 0) {
            offset = ((offset / ISO_SECTOR_SIZE) + 1) * ISO_SECTOR_SIZE;
            continue;
        }

        odfs_node_t node;
        int consumed = joliet_parse_dir_record(rec, avail, ctx->session_start,
                                               &ctx->next_node_id, &node);
        if (consumed == 0) {
            offset = ((offset / ISO_SECTOR_SIZE) + 1) * ISO_SECTOR_SIZE;
            continue;
        }

        node.parent_id = dir->id;

        if ((node.name[0] == '.' && node.name[1] == '\0') ||
            (node.name[0] == '.' && node.name[1] == '.' && node.name[2] == '\0')) {
            offset += consumed;
            continue;
        }

        offset += consumed;

        err = callback(&node, cb_ctx);
        if (err != ODFS_OK) {
            if (resume_offset)
                *resume_offset = offset;
            return err;
        }
    }

    if (resume_offset)
        *resume_offset = dir_size;
    return ODFS_OK;
}

/* ------------------------------------------------------------------ */
/* read                                                                */
/* ------------------------------------------------------------------ */

static odfs_err_t joliet_read(void *backend_ctx,
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

    if (offset >= file->size) { *len = 0; return ODFS_OK; }
    if (offset + want > file->size)
        want = (size_t)(file->size - offset);

    while (done < want) {
        uint64_t file_pos = offset + done;
        uint32_t sector_lba = file->extent.lba + (uint32_t)(file_pos / ISO_SECTOR_SIZE);
        uint32_t sector_off = (uint32_t)(file_pos % ISO_SECTOR_SIZE);
        const uint8_t *sector;
        odfs_err_t err;

        err = odfs_cache_read(cache, sector_lba, &sector);
        if (err != ODFS_OK) { *len = done; return err; }

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

typedef struct joliet_lookup_ctx {
    const char   *name;
    odfs_node_t *result;
    int           found;
} joliet_lookup_ctx_t;

static odfs_err_t joliet_lookup_cb(const odfs_node_t *entry, void *cb_ctx)
{
    joliet_lookup_ctx_t *lctx = cb_ctx;
    if (strcasecmp(entry->name, lctx->name) == 0) {
        *lctx->result = *entry;
        lctx->found = 1;
        return ODFS_ERR_EOF;
    }
    return ODFS_OK;
}

static odfs_err_t joliet_lookup(void *backend_ctx,
                                 odfs_cache_t *cache,
                                 odfs_log_state_t *log,
                                 const odfs_node_t *dir,
                                 const char *name,
                                 odfs_node_t *out)
{
    joliet_lookup_ctx_t lctx;
    odfs_err_t err;

    lctx.name = name;
    lctx.result = out;
    lctx.found = 0;

    err = joliet_readdir(backend_ctx, cache, log, dir, joliet_lookup_cb, &lctx, NULL);
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

static odfs_err_t joliet_get_volume_name(void *backend_ctx,
                                          char *buf, size_t buf_size)
{
    joliet_context_t *ctx = backend_ctx;
    size_t len = strlen(ctx->svd.volume_id);
    if (len >= buf_size) len = buf_size - 1;
    memcpy(buf, ctx->svd.volume_id, len);
    buf[len] = '\0';
    return ODFS_OK;
}

/* ------------------------------------------------------------------ */
/* backend ops table                                                   */
/* ------------------------------------------------------------------ */

const odfs_backend_ops_t joliet_backend_ops = {
    .name            = "joliet",
    .backend_type    = ODFS_BACKEND_JOLIET,
    .probe           = joliet_probe,
    .mount           = joliet_mount,
    .unmount         = joliet_unmount,
    .readdir         = joliet_readdir,
    .read            = joliet_read,
    .lookup          = joliet_lookup,
    .get_volume_name = joliet_get_volume_name,
};
