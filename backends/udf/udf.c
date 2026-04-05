/*
 * udf.c — UDF backend (read-only)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Supports UDF-only images and ISO/UDF bridge discs.
 * Reads AVDP, VDS, partition, FSD, and file entries.
 */

#include "udf.h"
#include "odfs/alloc.h"
#include "odfs/cache.h"
#include "odfs/charset.h"
#include "odfs/log.h"
#include "odfs/error.h"
#include "odfs/string.h"

#include <string.h>
#include <inttypes.h>

/* ------------------------------------------------------------------ */
/* tag parsing                                                         */
/* ------------------------------------------------------------------ */

static int udf_read_tag(const uint8_t *data, udf_tag_t *tag)
{
    tag->id         = udf_le16(&data[0]);
    tag->version    = udf_le16(&data[2]);
    tag->checksum   = data[4];
    tag->serial     = udf_le16(&data[6]);
    tag->crc        = udf_le16(&data[8]);
    tag->crc_length = udf_le16(&data[10]);
    tag->location   = udf_le32(&data[12]);

    /* verify tag checksum (sum of bytes 0-3,5-15 mod 256) */
    uint8_t sum = 0;
    for (int i = 0; i < 16; i++) {
        if (i != 4)
            sum += data[i];
    }
    return (sum == tag->checksum) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* OSTA CS0 compressed Unicode → UTF-8                                 */
/* ------------------------------------------------------------------ */

static void udf_decode_dstring(const uint8_t *src, size_t field_len,
                               char *dst, size_t dst_size)
{
    if (field_len == 0 || dst_size == 0) {
        if (dst_size > 0) dst[0] = '\0';
        return;
    }

    /* last byte of d-string is the used length */
    uint8_t used = src[field_len - 1];
    if (used == 0 || used > field_len - 1) {
        dst[0] = '\0';
        return;
    }

    uint8_t comp_id = src[0]; /* 8 = Latin-1, 16 = UCS-2 */
    size_t di = 0;

    if (comp_id == 8) {
        /* 8-bit characters (Latin-1) */
        for (size_t i = 1; i < used && di + 1 < dst_size; i++)
            dst[di++] = (char)src[i];
    } else if (comp_id == 16) {
        /* 16-bit UCS-2 BE */
        odfs_ucs2be_to_utf8(src + 1, used - 1,
                              dst, dst_size, &di);
    } else {
        /* unknown compression, copy raw */
        for (size_t i = 1; i < used && di + 1 < dst_size; i++)
            dst[di++] = (char)src[i];
    }

    dst[di] = '\0';

    /* trim trailing spaces */
    while (di > 0 && dst[di - 1] == ' ')
        dst[--di] = '\0';
}

/*
 * Decode a OSTA CS0 identifier (from FID name field).
 * Unlike d-strings, these don't have the length byte at the end.
 */
static void udf_decode_cs0(const uint8_t *src, size_t len,
                            char *dst, size_t dst_size)
{
    if (len == 0 || dst_size == 0) {
        if (dst_size > 0) dst[0] = '\0';
        return;
    }

    uint8_t comp_id = src[0];
    size_t di = 0;

    if (comp_id == 8) {
        for (size_t i = 1; i < len && di + 1 < dst_size; i++)
            dst[di++] = (char)src[i];
    } else if (comp_id == 16) {
        odfs_ucs2be_to_utf8(src + 1, len - 1,
                              dst, dst_size, &di);
    } else {
        for (size_t i = 1; i < len && di + 1 < dst_size; i++)
            dst[di++] = (char)src[i];
    }
    dst[di] = '\0';
}

/* ------------------------------------------------------------------ */
/* timestamp parsing                                                   */
/* ------------------------------------------------------------------ */

static void udf_parse_timestamp(const uint8_t *ts, odfs_timestamp_t *out)
{
    memset(out, 0, sizeof(*out));
    /* ECMA-167 timestamp: type(2) + tz(2) + year(2) + ... */
    int16_t tz = (int16_t)udf_le16(&ts[2]);
    out->year   = (int32_t)(int16_t)udf_le16(&ts[4]);
    out->month  = ts[6];
    out->day    = ts[7];
    out->hour   = ts[8];
    out->minute = ts[9];
    out->second = ts[10];
    /* tz is in minutes, -2047 to 2047; -2048 = unspecified */
    if (tz >= -1440 && tz <= 1440)
        out->tz_offset = tz;
    else
        out->tz_offset = 0;
}

/* ------------------------------------------------------------------ */
/* partition-relative LBA → physical LBA                              */
/* ------------------------------------------------------------------ */

static inline uint32_t udf_phys_lba(const udf_context_t *ctx, uint32_t part_lba)
{
    return ctx->part_start + part_lba;
}

/* ------------------------------------------------------------------ */
/* probe — look for AVDP at LBA 256                                    */
/* ------------------------------------------------------------------ */

static odfs_err_t udf_probe(odfs_cache_t *cache,
                              odfs_log_state_t *log,
                              uint32_t session_start)
{
    const uint8_t *sector;
    udf_tag_t tag;

    odfs_err_t err = odfs_cache_read(cache,
                                        session_start + UDF_AVDP_LBA,
                                        &sector);
    if (err != ODFS_OK)
        return err;

    if (!udf_read_tag(sector, &tag) || tag.id != UDF_TAG_AVDP) {
        ODFS_DEBUG(log, ODFS_SUB_UDF,
                    "no AVDP at LBA %" PRIu32,
                    session_start + UDF_AVDP_LBA);
        return ODFS_ERR_BAD_FORMAT;
    }

    ODFS_INFO(log, ODFS_SUB_UDF,
               "UDF AVDP found at LBA %" PRIu32,
               session_start + UDF_AVDP_LBA);
    return ODFS_OK;
}

/* ------------------------------------------------------------------ */
/* mount                                                               */
/* ------------------------------------------------------------------ */

static odfs_err_t udf_mount(odfs_cache_t *cache,
                              odfs_log_state_t *log,
                              uint32_t session_start,
                              odfs_node_t *root_out,
                              void **backend_ctx)
{
    udf_context_t *ctx;
    const uint8_t *sector;
    udf_tag_t tag;
    odfs_err_t err;

    ctx = odfs_calloc(1, sizeof(*ctx));
    if (!ctx)
        return ODFS_ERR_NOMEM;

    ctx->next_node_id = 1;
    ctx->lv_block_size = 2048;

    /* read AVDP */
    err = odfs_cache_read(cache, session_start + UDF_AVDP_LBA, &sector);
    if (err != ODFS_OK) { odfs_free(ctx); return err; }

    if (!udf_read_tag(sector, &tag) || tag.id != UDF_TAG_AVDP) {
        odfs_free(ctx);
        return ODFS_ERR_BAD_FORMAT;
    }

    /* AVDP: main VDS extent at offset 16 */
    uint32_t mvds_lba = udf_le32(&sector[16 + 4]);
    uint32_t mvds_len = udf_le32(&sector[16]);
    uint32_t mvds_sectors = mvds_len / 2048;

    ODFS_DEBUG(log, ODFS_SUB_UDF,
                "MVDS at LBA %" PRIu32 ", %" PRIu32 " sectors",
                mvds_lba, mvds_sectors);

    /* scan VDS for PVD, PD, LVD */
    int found_pd = 0, found_lvd = 0;
    udf_long_ad_t fsd_ad;
    memset(&fsd_ad, 0, sizeof(fsd_ad));

    for (uint32_t i = 0; i < mvds_sectors; i++) {
        err = odfs_cache_read(cache, mvds_lba + i, &sector);
        if (err != ODFS_OK)
            continue;

        if (!udf_read_tag(sector, &tag))
            continue;

        switch (tag.id) {
        case UDF_TAG_PVD:
            /* Primary Volume Descriptor: volume id at offset 24, 32 bytes d-string */
            udf_decode_dstring(&sector[24], 32,
                               ctx->volume_id, sizeof(ctx->volume_id));
            ODFS_INFO(log, ODFS_SUB_UDF,
                       "PVD volume: \"%s\"", ctx->volume_id);
            break;

        case UDF_TAG_PD:
            /* Partition Descriptor */
            ctx->part_number = udf_le16(&sector[22]);
            ctx->part_start  = udf_le32(&sector[188]);
            ctx->part_length = udf_le32(&sector[192]);
            found_pd = 1;
            ODFS_INFO(log, ODFS_SUB_UDF,
                       "partition %u: start LBA %" PRIu32 ", %" PRIu32 " sectors",
                       ctx->part_number, ctx->part_start, ctx->part_length);
            break;

        case UDF_TAG_LVD:
            /* Logical Volume Descriptor */
            ctx->lv_block_size = udf_le32(&sector[212]);
            /* LVD volume id at offset 84, 128 bytes d-string */
            if (ctx->volume_id[0] == '\0')
                udf_decode_dstring(&sector[84], 128,
                                   ctx->volume_id, sizeof(ctx->volume_id));

            /* FSD location: long_ad at offset 248 within the LVD.
             * But LVD is variable-length. The map table starts at 440.
             * The FSD long_ad is at a fixed offset in the LVD:
             * ECMA-167: Logical Volume Contents Use at offset 248, 16 bytes long_ad.
             */
            fsd_ad.length    = udf_le32(&sector[248]);
            fsd_ad.lba       = udf_le32(&sector[252]);
            fsd_ad.partition  = udf_le16(&sector[256]);
            found_lvd = 1;
            ODFS_INFO(log, ODFS_SUB_UDF,
                       "LVD block size: %" PRIu32 ", FSD at part LBA %" PRIu32,
                       ctx->lv_block_size, fsd_ad.lba);
            break;

        case UDF_TAG_TD:
            goto vds_done;

        default:
            break;
        }
    }
vds_done:

    if (!found_pd || !found_lvd) {
        ODFS_ERROR(log, ODFS_SUB_UDF,
                    "incomplete VDS: PD=%d LVD=%d", found_pd, found_lvd);
        odfs_free(ctx);
        return ODFS_ERR_BAD_FORMAT;
    }

    /* read File Set Descriptor */
    uint32_t fsd_phys = udf_phys_lba(ctx, fsd_ad.lba);
    err = odfs_cache_read(cache, fsd_phys, &sector);
    if (err != ODFS_OK) { odfs_free(ctx); return err; }

    if (!udf_read_tag(sector, &tag) || tag.id != UDF_TAG_FSD) {
        ODFS_ERROR(log, ODFS_SUB_UDF,
                    "FSD not found at LBA %" PRIu32, fsd_phys);
        odfs_free(ctx);
        return ODFS_ERR_BAD_FORMAT;
    }

    /* root directory ICB: long_ad at FSD offset 400 */
    ctx->root_icb_lba  = udf_le32(&sector[404]);
    ctx->root_icb_part = udf_le16(&sector[408]);

    ODFS_INFO(log, ODFS_SUB_UDF,
               "root ICB at part LBA %" PRIu32, ctx->root_icb_lba);

    /* read root ICB (File Entry or Extended File Entry) */
    uint32_t root_phys = udf_phys_lba(ctx, ctx->root_icb_lba);
    err = odfs_cache_read(cache, root_phys, &sector);
    if (err != ODFS_OK) { odfs_free(ctx); return err; }

    if (!udf_read_tag(sector, &tag) ||
        (tag.id != UDF_TAG_FE && tag.id != UDF_TAG_EFE)) {
        ODFS_ERROR(log, ODFS_SUB_UDF,
                    "root ICB tag %" PRIu16 " (expected FE/EFE)", tag.id);
        odfs_free(ctx);
        return ODFS_ERR_BAD_FORMAT;
    }

    /* build root node */
    memset(root_out, 0, sizeof(*root_out));
    root_out->id         = 0;
    root_out->parent_id  = 0;
    root_out->backend    = ODFS_BACKEND_UDF;
    root_out->kind       = ODFS_NODE_DIR;
    root_out->name[0]    = '/';
    root_out->name[1]    = '\0';

    if (tag.id == UDF_TAG_FE) {
        root_out->size = udf_le64(&sector[56]);
        udf_parse_timestamp(&sector[84], &root_out->mtime);
    } else { /* EFE */
        root_out->size = udf_le64(&sector[56]);
        udf_parse_timestamp(&sector[108], &root_out->mtime);
    }
    root_out->ctime = root_out->mtime;

    /* store extent as the ICB location (for readdir) */
    root_out->extent.lba    = root_phys;
    root_out->extent.length = (uint32_t)root_out->size;

    *backend_ctx = ctx;
    return ODFS_OK;
}

/* ------------------------------------------------------------------ */
/* unmount                                                             */
/* ------------------------------------------------------------------ */

static void udf_unmount(void *backend_ctx)
{
    odfs_free(backend_ctx);
}

/* ------------------------------------------------------------------ */
/* read file entry and get data extent                                 */
/* ------------------------------------------------------------------ */

/*
 * Read a File Entry (or Extended File Entry) ICB and extract
 * the data location and size. Returns the physical LBA and
 * byte length of the file's data.
 */
static odfs_err_t udf_read_icb(udf_context_t *ctx,
                                odfs_cache_t *cache,
                                uint32_t icb_phys_lba,
                                uint64_t *data_size,
                                uint32_t *data_phys_lba,
                                uint8_t *file_type,
                                odfs_timestamp_t *mtime)
{
    const uint8_t *sector;
    udf_tag_t tag;

    odfs_err_t err = odfs_cache_read(cache, icb_phys_lba, &sector);
    if (err != ODFS_OK)
        return err;

    if (!udf_read_tag(sector, &tag))
        return ODFS_ERR_CORRUPT;

    uint16_t alloc_type;
    uint32_t ad_offset;  /* offset of allocation descriptors in FE */
    uint32_t ad_length;

    if (tag.id == UDF_TAG_FE) {
        /* File Entry (ECMA-167 14.9) */
        *file_type = sector[11]; /* ICB tag file type at offset 11 within ICB tag (FE+16) */
        /* Actually ICB tag is at FE offset 16, file type at ICB tag offset 11 */
        *file_type = sector[16 + 11];
        alloc_type = udf_le16(&sector[16 + 18]) & 0x07;
        *data_size = udf_le64(&sector[56]);
        if (mtime)
            udf_parse_timestamp(&sector[84], mtime);
        uint32_t ea_len = udf_le32(&sector[168]);
        ad_length = udf_le32(&sector[172]);
        ad_offset = 176 + ea_len;
    } else if (tag.id == UDF_TAG_EFE) {
        /* Extended File Entry (ECMA-167 14.17) */
        *file_type = sector[16 + 11];
        alloc_type = udf_le16(&sector[16 + 18]) & 0x07;
        *data_size = udf_le64(&sector[56]);
        if (mtime)
            udf_parse_timestamp(&sector[108], mtime);
        uint32_t ea_len = udf_le32(&sector[208]);
        ad_length = udf_le32(&sector[212]);
        ad_offset = 216 + ea_len;
    } else {
        return ODFS_ERR_BAD_FORMAT;
    }

    /* get data location from first allocation descriptor */
    *data_phys_lba = 0;

    if (alloc_type == UDF_ICB_ALLOC_EMBEDDED) {
        /* data is embedded in the FE itself */
        *data_phys_lba = icb_phys_lba;
    } else if (alloc_type == UDF_ICB_ALLOC_SHORT && ad_length >= 8) {
        /* short AD: 4 bytes length + 4 bytes position */
        uint32_t pos = udf_le32(&sector[ad_offset + 4]);
        *data_phys_lba = udf_phys_lba(ctx, pos);
    } else if (alloc_type == UDF_ICB_ALLOC_LONG && ad_length >= 16) {
        /* long AD: 4 bytes length + 4 bytes LBA + 2 bytes partition + 6 impl use */
        uint32_t lba = udf_le32(&sector[ad_offset + 4]);
        *data_phys_lba = udf_phys_lba(ctx, lba);
    }

    return ODFS_OK;
}

/* ------------------------------------------------------------------ */
/* readdir                                                             */
/* ------------------------------------------------------------------ */

static odfs_err_t udf_readdir(void *backend_ctx,
                                odfs_cache_t *cache,
                                odfs_log_state_t *log,
                                const odfs_node_t *dir,
                                odfs_dir_iter_fn callback,
                                void *cb_ctx,
                                uint32_t *resume_offset)
{
    udf_context_t *ctx = backend_ctx;
    uint64_t dir_size;
    uint32_t dir_data_lba;
    uint8_t dir_ftype;
    odfs_err_t err;

    (void)log;

    /* read the directory's ICB to find its data extent */
    err = udf_read_icb(ctx, cache, dir->extent.lba,
                       &dir_size, &dir_data_lba, &dir_ftype, NULL);
    if (err != ODFS_OK)
        return err;

    uint32_t offset = (resume_offset && *resume_offset) ? *resume_offset : 0;

    /* walk File Identifier Descriptors */
    while (offset < dir_size) {
        uint32_t sector_lba = dir_data_lba + (offset / 2048);
        uint32_t sector_off = offset % 2048;
        const uint8_t *sector;

        err = odfs_cache_read(cache, sector_lba, &sector);
        if (err != ODFS_OK)
            return err;

        const uint8_t *fid = sector + sector_off;
        size_t avail = 2048 - sector_off;

        if (avail < 38) {
            /* FID header doesn't fit in remainder — skip to next sector */
            offset = ((offset / 2048) + 1) * 2048;
            continue;
        }

        udf_tag_t tag;
        if (!udf_read_tag(fid, &tag) || tag.id != UDF_TAG_FID)
            break;

        uint16_t fid_version = udf_le16(&fid[16]);
        uint8_t fid_flags    = fid[18];
        uint8_t name_len     = fid[19];
        uint32_t icb_lba     = udf_le32(&fid[20 + 4]);
        uint16_t icb_part    = udf_le16(&fid[20 + 8]);
        uint16_t impl_len    = udf_le16(&fid[36]);
        (void)fid_version;
        (void)icb_part;

        /* FID total length: 38 + impl_len + name_len, padded to 4 bytes */
        uint32_t fid_len = (38 + impl_len + name_len + 3) & ~3u;

        if (fid_len > avail || fid_len < 38) {
            /* FID spans sector boundary — skip for now */
            /* TODO: handle cross-sector FIDs */
            offset = ((offset / 2048) + 1) * 2048;
            continue;
        }

        /* skip deleted and parent entries */
        if (fid_flags & UDF_FID_FLAG_DELETED) {
            offset += fid_len;
            continue;
        }
        if (fid_flags & UDF_FID_FLAG_PARENT) {
            offset += fid_len;
            continue;
        }

        /* decode filename */
        odfs_node_t node;
        memset(&node, 0, sizeof(node));
        node.id = ctx->next_node_id++;
        node.parent_id = dir->id;
        node.backend = ODFS_BACKEND_UDF;

        if (name_len > 0) {
            const uint8_t *name_data = fid + 38 + impl_len;
            udf_decode_cs0(name_data, name_len,
                           node.name, sizeof(node.name));
        }

        /* read the file's ICB for metadata */
        uint32_t icb_phys = udf_phys_lba(ctx, icb_lba);
        uint64_t fsize = 0;
        uint32_t data_lba = 0;
        uint8_t ftype = 0;
        odfs_timestamp_t ts;
        memset(&ts, 0, sizeof(ts));

        err = udf_read_icb(ctx, cache, icb_phys,
                           &fsize, &data_lba, &ftype, &ts);
        if (err != ODFS_OK) {
            offset += fid_len;
            continue;
        }

        node.size = fsize;
        node.mtime = ts;
        node.ctime = ts;
        node.extent.lba = icb_phys; /* store ICB LBA for later reads */
        node.extent.length = (uint32_t)fsize;

        if (fid_flags & UDF_FID_FLAG_DIRECTORY)
            node.kind = ODFS_NODE_DIR;
        else if (ftype == UDF_ICB_FILETYPE_SYMLINK)
            node.kind = ODFS_NODE_SYMLINK;
        else
            node.kind = ODFS_NODE_FILE;

        offset += fid_len;

        err = callback(&node, cb_ctx);
        if (err != ODFS_OK) {
            if (resume_offset)
                *resume_offset = offset;
            return err;
        }
    }

    if (resume_offset)
        *resume_offset = (uint32_t)dir_size;
    return ODFS_OK;
}

/* ------------------------------------------------------------------ */
/* read                                                                */
/* ------------------------------------------------------------------ */

static odfs_err_t udf_read(void *backend_ctx,
                             odfs_cache_t *cache,
                             odfs_log_state_t *log,
                             const odfs_node_t *file,
                             uint64_t offset,
                             void *buf,
                             size_t *len)
{
    udf_context_t *ctx = backend_ctx;
    uint64_t fsize;
    uint32_t data_lba;
    uint8_t ftype;
    odfs_err_t err;
    (void)log;

    /* read ICB to get data extent */
    err = udf_read_icb(ctx, cache, file->extent.lba,
                       &fsize, &data_lba, &ftype, NULL);
    if (err != ODFS_OK)
        return err;

    size_t want = *len;
    if (offset >= fsize) { *len = 0; return ODFS_OK; }
    if (offset + want > fsize)
        want = (size_t)(fsize - offset);

    size_t done = 0;
    uint8_t *out = buf;

    while (done < want) {
        uint64_t pos = offset + done;
        uint32_t slba = data_lba + (uint32_t)(pos / 2048);
        uint32_t soff = (uint32_t)(pos % 2048);
        const uint8_t *sector;

        err = odfs_cache_read(cache, slba, &sector);
        if (err != ODFS_OK) { *len = done; return err; }

        size_t chunk = 2048 - soff;
        if (chunk > want - done)
            chunk = want - done;

        memcpy(out + done, sector + soff, chunk);
        done += chunk;
    }

    *len = done;
    return ODFS_OK;
}

/* ------------------------------------------------------------------ */
/* lookup                                                              */
/* ------------------------------------------------------------------ */

typedef struct udf_lookup_ctx {
    const char   *name;
    odfs_node_t *result;
    int           found;
} udf_lookup_ctx_t;

static odfs_err_t udf_lookup_cb(const odfs_node_t *entry, void *cb_ctx)
{
    udf_lookup_ctx_t *lctx = cb_ctx;
    if (odfs_strcasecmp(entry->name, lctx->name) == 0) {
        *lctx->result = *entry;
        lctx->found = 1;
        return ODFS_ERR_EOF;
    }
    return ODFS_OK;
}

static odfs_err_t udf_lookup(void *backend_ctx,
                               odfs_cache_t *cache,
                               odfs_log_state_t *log,
                               const odfs_node_t *dir,
                               const char *name,
                               odfs_node_t *out)
{
    udf_lookup_ctx_t lctx;
    odfs_err_t err;

    lctx.name = name;
    lctx.result = out;
    lctx.found = 0;

    err = udf_readdir(backend_ctx, cache, log, dir, udf_lookup_cb, &lctx, NULL);
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

static odfs_err_t udf_get_volume_name(void *backend_ctx,
                                        char *buf, size_t buf_size)
{
    udf_context_t *ctx = backend_ctx;
    size_t len = strlen(ctx->volume_id);
    if (len >= buf_size) len = buf_size - 1;
    memcpy(buf, ctx->volume_id, len);
    buf[len] = '\0';
    return ODFS_OK;
}

/* ------------------------------------------------------------------ */
/* backend ops table                                                   */
/* ------------------------------------------------------------------ */

const odfs_backend_ops_t udf_backend_ops = {
    .name            = "udf",
    .backend_type    = ODFS_BACKEND_UDF,
    .probe           = udf_probe,
    .mount           = udf_mount,
    .unmount         = udf_unmount,
    .readdir         = udf_readdir,
    .read            = udf_read,
    .lookup          = udf_lookup,
    .get_volume_name = udf_get_volume_name,
};
