/*
 * iso9660.c — ISO 9660 backend implementation
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "iso9660.h"
#include "odfs/alloc.h"
#include "odfs/cache.h"
#include "odfs/charset.h"
#include "odfs/namefix.h"
#include "odfs/log.h"
#include "odfs/node.h"
#include "odfs/error.h"
#include "odfs/string.h"

#if ODFS_FEATURE_ROCK_RIDGE
#include "rock_ridge/rock_ridge.h"
#endif

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

#if ODFS_FEATURE_ROCK_RIDGE
static void iso_apply_rr(odfs_node_t *node, const rr_info_t *rr,
                         uint32_t session_start, int allow_name)
{
    if (allow_name && rr->has_name && rr->name[0] != '\0') {
        size_t nlen = strlen(rr->name);
        if (nlen >= sizeof(node->name))
            nlen = sizeof(node->name) - 1;
        memcpy(node->name, rr->name, nlen);
        node->name[nlen] = '\0';
    }

    if (rr->has_posix) {
        node->mode = rr->mode;
        if ((rr->mode & 0170000) == 0120000)
            node->kind = ODFS_NODE_SYMLINK;
        else if ((rr->mode & 0170000) == 0040000)
            node->kind = ODFS_NODE_DIR;
        else
            node->kind = ODFS_NODE_FILE;
    }

    if (rr->has_timestamps) {
        node->mtime = rr->mtime;
        node->ctime = rr->ctime;
    }

    if (rr->has_child_link)
        node->extent.lba = rr->child_link_lba + session_start;

    if (rr->has_amiga_protection) {
        memcpy(node->amiga.protection, rr->amiga_protection, 4);
        node->amiga.has_protection = 1;
    }

    if (rr->has_amiga_comment) {
        size_t clen = strlen(rr->amiga_comment);
        if (clen >= sizeof(node->amiga.comment))
            clen = sizeof(node->amiga.comment) - 1;
        memcpy(node->amiga.comment, rr->amiga_comment, clen);
        node->amiga.comment[clen] = '\0';
        node->amiga.has_comment = 1;
    }

    node->backend = ODFS_BACKEND_ROCK_RIDGE;
}
#endif

/* iso_copy_strfield is now in iso9660.h as static inline */

/*
 * Parse a directory record at the given pointer into an odfs_node_t.
 * Returns the total record length consumed, or 0 on error / end of records.
 * If sua_out/sua_len_out are non-NULL, returns a pointer to and length of
 * the System Use Area (for Rock Ridge parsing).
 */
static int iso_parse_dir_record(const uint8_t *data, size_t avail,
                                uint32_t session_start,
                                uint32_t *next_id,
                                int lowercase,
                                odfs_node_t *node,
                                const uint8_t **sua_out,
                                size_t *sua_len_out)
{
    uint8_t rec_len;
    uint8_t name_len;
    uint8_t flags;
    size_t name_end;

    if (sua_out) *sua_out = NULL;
    if (sua_len_out) *sua_len_out = 0;

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
        node->name[0] = '.';
        node->name[1] = '\0';
    } else if (name_len == 1 && data[ISO_DR_NAME] == 0x01) {
        node->name[0] = '.';
        node->name[1] = '.';
        node->name[2] = '\0';
    } else {
        odfs_iso_name_to_display((const char *)&data[ISO_DR_NAME], name_len,
                                  node->name, sizeof(node->name),
                                  lowercase);
    }

    /* System Use Area starts after filename, padded to even offset.
     * ECMA-119: if name_len is even, a pad byte follows the name. */
    name_end = 33 + name_len;
    if ((name_len & 1) == 0)
        name_end++; /* pad byte after even-length filename */
    if (name_end < rec_len && sua_out && sua_len_out) {
        *sua_out = &data[name_end];
        *sua_len_out = rec_len - name_end;
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

    ctx = odfs_calloc(1, sizeof(*ctx));
    if (!ctx)
        return ODFS_ERR_NOMEM;

    ctx->session_start = session_start;
    ctx->next_node_id = 1;
    ctx->lowercase = 0; /* preserve original case by default */

    /* read PVD */
    err = odfs_cache_read(cache, session_start + ISO_VD_START_LBA, &sector);
    if (err != ODFS_OK) {
        odfs_free(ctx);
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

    /*
     * Multisession: if the PVD's root LBA is already >= session_start,
     * the mastering tool wrote absolute LBAs (e.g. mkisofs -C).
     * Don't add session_start again. Only add it for session-relative LBAs.
     */
    if (ctx->pvd.root_dir_lba < session_start)
        ctx->pvd.root_dir_lba += session_start;

    /*
     * If the on-disc LBAs are already absolute, set session_start to 0
     * in the context so readdir doesn't double-offset file extents.
     */
    if (session_start > 0 && ctx->pvd.root_dir_lba >= session_start) {
        ctx->session_start = 0;
    }

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

#if ODFS_FEATURE_ROCK_RIDGE
    /* detect Rock Ridge by reading the root directory's "." entry */
    {
        const uint8_t *root_sector;
        err = odfs_cache_read(cache, ctx->pvd.root_dir_lba, &root_sector);
        if (err == ODFS_OK) {
            /* the "." entry is the first record in the root directory */
            uint8_t dot_rec_len = root_sector[ISO_DR_LENGTH];
            uint8_t dot_name_len = root_sector[ISO_DR_NAME_LEN];
            if (dot_rec_len >= 34 && dot_name_len == 1) {
                size_t sua_start = 34; /* 33 + 1 byte name, already even */
                if (sua_start < dot_rec_len) {
                    const uint8_t *sua = root_sector + sua_start;
                    size_t sua_len = dot_rec_len - sua_start;
                    int rr_skip = 0;
                    if (rr_detect(sua, sua_len, &rr_skip)) {
                        rr_info_t rr;

                        ctx->has_rock_ridge = 1;
                        ctx->rr_skip = rr_skip;
                        root_out->backend = ODFS_BACKEND_ROCK_RIDGE;
                        ODFS_INFO(log, ODFS_SUB_RR,
                                   "Rock Ridge extensions detected (skip=%d)",
                                   rr_skip);

                        rr_parse(sua, sua_len, rr_skip, &rr, cache);
                        iso_apply_rr(root_out, &rr, ctx->session_start, 0);
                    }
                }
            }
        }
    }
#endif

    *backend_ctx = ctx;
    return ODFS_OK;
}

/* ------------------------------------------------------------------ */
/* unmount                                                             */
/* ------------------------------------------------------------------ */

static void iso_unmount(void *backend_ctx)
{
    odfs_free(backend_ctx);
}

/* ------------------------------------------------------------------ */
/* readdir                                                             */
/* ------------------------------------------------------------------ */

static odfs_err_t iso_readdir(void *backend_ctx,
                               odfs_cache_t *cache,
                               odfs_log_state_t *log,
                               const odfs_node_t *dir,
                               odfs_dir_iter_fn callback,
                               void *cb_ctx,
                               uint32_t *resume_offset)
{
    iso_context_t *ctx = backend_ctx;
    uint32_t dir_lba = dir->extent.lba;
    uint32_t dir_size = dir->extent.length;
    uint32_t target_offset = (resume_offset && *resume_offset) ? *resume_offset : 0;
    uint32_t offset = 0;
    int lowercase = ctx->lowercase;
    odfs_namefix_state_t namefix;

    ODFS_TRACE(log, ODFS_SUB_ISO,
                "readdir: LBA %" PRIu32 ", size %" PRIu32, dir_lba, dir_size);
    odfs_namefix_init(&namefix);

    while (offset < dir_size) {
        uint32_t entry_start = offset;
        uint32_t sector_lba = dir_lba + (offset / ISO_SECTOR_SIZE);
        uint32_t sector_off = offset % ISO_SECTOR_SIZE;
        const uint8_t *sector;
        odfs_err_t err;

        err = odfs_cache_read(cache, sector_lba, &sector);
        if (err != ODFS_OK) {
            odfs_namefix_destroy(&namefix);
            return err;
        }

        const uint8_t *rec = sector + sector_off;
        size_t avail = ISO_SECTOR_SIZE - sector_off;

        /* zero-length record means skip to next sector */
        if (rec[0] == 0) {
            offset = ((offset / ISO_SECTOR_SIZE) + 1) * ISO_SECTOR_SIZE;
            continue;
        }

        odfs_node_t node;
        const uint8_t *sua = NULL;
        size_t sua_len = 0;
        int consumed = iso_parse_dir_record(rec, avail, ctx->session_start,
                                            &ctx->next_node_id, lowercase,
                                            &node, &sua, &sua_len);
        if (consumed == 0) {
            offset = ((offset / ISO_SECTOR_SIZE) + 1) * ISO_SECTOR_SIZE;
            continue;
        }

        node.parent_id = dir->id;

#if ODFS_FEATURE_ROCK_RIDGE
        /* apply Rock Ridge overrides */
        if (ctx->has_rock_ridge && sua && sua_len > 0) {
            rr_info_t rr;
            rr_parse(sua, sua_len, ctx->rr_skip, &rr, cache);

            /* skip relocated entries (RE) */
            if (rr.is_relocated) {
                offset += consumed;
                continue;
            }
            iso_apply_rr(&node, &rr, ctx->session_start, 1);
        }
#endif

        /* skip . and .. entries */
        if ((node.name[0] == '.' && node.name[1] == '\0') ||
            (node.name[0] == '.' && node.name[1] == '.' && node.name[2] == '\0')) {
            offset += consumed;
            continue;
        }

        err = odfs_namefix_apply(&namefix, node.name, sizeof(node.name));
        if (err != ODFS_OK) {
            odfs_namefix_destroy(&namefix);
            return err;
        }

        /* advance offset BEFORE callback so resume_offset points
           to the entry AFTER the one we're about to deliver */
        offset += consumed;

        if (entry_start < target_offset)
            continue;

        err = callback(&node, cb_ctx);
        if (err != ODFS_OK) {
            /* callback stopped iteration — save resume point */
            if (resume_offset)
                *resume_offset = offset;
            odfs_namefix_destroy(&namefix);
            return err;
        }
    }

    if (resume_offset)
        *resume_offset = dir_size; /* exhausted */
    odfs_namefix_destroy(&namefix);
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
    if (odfs_strcasecmp(entry->name, lctx->name) == 0) {
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

    err = iso_readdir(backend_ctx, cache, log, dir, lookup_cb, &lctx, NULL);
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
