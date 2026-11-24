/*
 * udf.c — UDF backend (read-only)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Supports UDF-only images and ISO/UDF bridge discs, including the
 * metadata partitions of UDF 2.50 and later.
 * Reads AVDP, VDS, partition, FSD, and file entries.
 */

#include "udf.h"
#include "odfs/alloc.h"
#include "odfs/cache.h"
#include "odfs/charset.h"
#include "odfs/namefix.h"
#include "odfs/log.h"
#include "odfs/error.h"
#include "odfs/string.h"

#include <string.h>
#include <inttypes.h>

/* the only supported logical block size: one optical media sector */
#define UDF_BLOCK_SIZE 2048u

/* the Main VDS needs at least 16 sectors; anything beyond this is corrupt */
#define UDF_MAX_VDS_SECTORS 256u

/* ------------------------------------------------------------------ */
/* tag parsing                                                         */
/* ------------------------------------------------------------------ */

/*
 * Return the descriptor tag identifier, or 0 (not a valid identifier)
 * if the tag checksum does not match.
 */
static __attribute__((noinline)) uint16_t udf_tag_id(const uint8_t *data)
{
    /* verify tag checksum (sum of bytes 0-3,5-15 mod 256) */
    uint32_t sum = 0;
    for (int i = 0; i < 16; i++) {
        if (i != 4)
            sum += data[i];
    }
    return ((uint8_t)sum == data[4]) ? udf_le16(&data[0]) : 0;
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
    size_t used = src[field_len - 1];
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
    /*
     * ECMA-167 1/7.3: type and time zone (2), year (2), month, day,
     * hour, minute, second, then three bytes of fractions. The time
     * zone is the low 12 bits of the first field: minutes from UTC as
     * a signed number, or -2047 for "not specified".
     */
    int tz = (int)((udf_le16(&ts[0]) & 0x0FFFu) ^ 0x0800u) - 0x0800;

    memset(out, 0, sizeof(*out));
    out->year   = (int32_t)(int16_t)udf_le16(&ts[2]);
    out->month  = ts[4];
    out->day    = ts[5];
    out->hour   = ts[6];
    out->minute = ts[7];
    out->second = ts[8];
    if (tz >= -1440 && tz <= 1440)
        out->tz_offset = (int16_t)tz;
}

/* ------------------------------------------------------------------ */
/* partition-relative LBA → physical LBA                              */
/* ------------------------------------------------------------------ */

/*
 * Translate block lba of partition reference part_ref to a media LBA.
 * Every reference other than the metadata partition's addresses the
 * one physical partition. If run_out is given, it receives the number
 * of blocks that are contiguous on the media from there: the metadata
 * partition is contiguous only within one Metadata File extent.
 */
static __attribute__((noinline)) odfs_err_t
udf_part_to_phys(const udf_context_t *ctx,
                 uint16_t part_ref,
                 uint32_t lba,
                 uint32_t *phys_out,
                 uint32_t *run_out)
{
    if (!ctx->has_meta_part || part_ref != ctx->meta_part_ref) {
        *phys_out = ctx->part_start + lba;
        if (run_out)
            *run_out = UINT32_MAX;
        return ODFS_OK;
    }

    for (uint32_t i = 0; i < ctx->meta_ext_count; i++) {
        const udf_meta_extent_t *ext = &ctx->meta_extents[i];

        if (lba < ext->blocks) {
            if (ext->phys_lba == UDF_META_HOLE)
                return ODFS_ERR_CORRUPT;
            *phys_out = ext->phys_lba + lba;
            if (run_out)
                *run_out = ext->blocks - lba;
            return ODFS_OK;
        }
        lba -= ext->blocks;
    }
    return ODFS_ERR_CORRUPT;
}

/*
 * Partition reference that short allocation descriptors of the ICB at
 * icb_phys refer to: the partition the ICB itself is recorded in.
 */
static uint16_t udf_icb_part(const udf_context_t *ctx, uint32_t icb_phys)
{
    for (uint32_t i = 0; ctx->has_meta_part && i < ctx->meta_ext_count; i++) {
        const udf_meta_extent_t *ext = &ctx->meta_extents[i];

        if (ext->phys_lba != UDF_META_HOLE && icb_phys >= ext->phys_lba &&
            icb_phys - ext->phys_lba < ext->blocks)
            return ctx->meta_part_ref;
    }
    return ctx->phys_part_ref;
}

/* ------------------------------------------------------------------ */
/* probe — look for AVDP at LBA 256                                    */
/* ------------------------------------------------------------------ */

static odfs_err_t udf_probe(odfs_cache_t *cache,
                              odfs_log_state_t *log,
                              uint32_t session_start)
{
    const uint8_t *sector;

    odfs_err_t err = odfs_cache_read(cache,
                                        session_start + UDF_AVDP_LBA,
                                        &sector);
    if (err != ODFS_OK)
        return err;

    if (udf_tag_id(sector) != UDF_TAG_AVDP) {
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

#define UDF_MAX_PDS 4

static odfs_err_t udf_load_meta_file(udf_context_t *ctx,
                                     odfs_cache_t *cache,
                                     odfs_log_state_t *log,
                                     uint32_t file_lba,
                                     uint32_t mirror_lba);

static odfs_err_t udf_mount(odfs_cache_t *cache,
                              odfs_log_state_t *log,
                              uint32_t session_start,
                              odfs_node_t *root_out,
                              void **backend_ctx)
{
    udf_context_t *ctx;
    const uint8_t *sector;
    uint32_t lv_block_size = 0;
    uint16_t pd_numbers[UDF_MAX_PDS];
    uint32_t pd_starts[UDF_MAX_PDS];
    uint32_t pd_lengths[UDF_MAX_PDS];
    int n_pds = 0, pick = -1;
    int have_part_number = 0;
    uint16_t part_number = 0;
    uint32_t meta_file_lba = 0, meta_mirror_lba = 0;
    uint16_t tag;
    odfs_err_t err;

    ctx = odfs_calloc(1, sizeof(*ctx));
    if (!ctx)
        return ODFS_ERR_NOMEM;

    /* read AVDP */
    err = odfs_cache_read(cache, session_start + UDF_AVDP_LBA, &sector);
    if (err != ODFS_OK) { odfs_free(ctx); return err; }

    if (udf_tag_id(sector) != UDF_TAG_AVDP) {
        odfs_free(ctx);
        return ODFS_ERR_BAD_FORMAT;
    }

    /* AVDP: main VDS extent at offset 16 */
    uint32_t mvds_lba = udf_le32(&sector[16 + 4]);
    uint32_t mvds_len = udf_le32(&sector[16]);
    uint32_t mvds_sectors = mvds_len / UDF_BLOCK_SIZE;

    ODFS_DEBUG(log, ODFS_SUB_UDF,
                "MVDS at LBA %" PRIu32 ", %" PRIu32 " sectors",
                mvds_lba, mvds_sectors);
    if (mvds_sectors > UDF_MAX_VDS_SECTORS)
        mvds_sectors = UDF_MAX_VDS_SECTORS;

    /* scan VDS for PVD, PD, LVD */
    int found_lvd = 0;
    udf_long_ad_t fsd_ad;
    memset(&fsd_ad, 0, sizeof(fsd_ad));

    for (uint32_t i = 0; i < mvds_sectors; i++) {
        err = odfs_cache_read(cache, mvds_lba + i, &sector);
        if (err != ODFS_OK)
            continue;

        switch (udf_tag_id(sector)) {
        case UDF_TAG_PVD:
            /* Primary Volume Descriptor: volume id at offset 24, 32 bytes d-string */
            udf_decode_dstring(&sector[24], 32,
                               ctx->volume_id, sizeof(ctx->volume_id));
            ODFS_INFO(log, ODFS_SUB_UDF,
                       "PVD volume: \"%s\"", ctx->volume_id);
            break;

        case UDF_TAG_PD:
            /* Partition Descriptor; the LVD's maps say which one is used */
            if (n_pds == UDF_MAX_PDS)
                break;
            pd_numbers[n_pds] = udf_le16(&sector[22]);
            pd_starts[n_pds]  = udf_le32(&sector[188]);
            pd_lengths[n_pds] = udf_le32(&sector[192]);
            ODFS_INFO(log, ODFS_SUB_UDF,
                       "partition %u: start LBA %" PRIu32 ", %" PRIu32 " sectors",
                       pd_numbers[n_pds], pd_starts[n_pds], pd_lengths[n_pds]);
            n_pds++;
            break;

        case UDF_TAG_LVD:
            /* Logical Volume Descriptor */
            lv_block_size = udf_le32(&sector[212]);
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
                       lv_block_size, fsd_ad.lba);

            /*
             * Partition maps start at 440; a map's index is the
             * partition reference that long_ads and FIDs use. Type 1
             * maps a physical partition. A Type 2 "*UDF Metadata
             * Partition" map (UDF 2.50+) maps a virtual partition onto
             * the same physical one through the Metadata File.
             */
            {
                uint32_t mt_len = udf_le32(&sector[264]);
                uint32_t n_maps = udf_le32(&sector[268]);
                uint32_t pm_off = 0;

                if (mt_len > UDF_BLOCK_SIZE - 440u)
                    mt_len = UDF_BLOCK_SIZE - 440u;

                for (uint32_t ref = 0;
                     ref < n_maps && pm_off + 2u <= mt_len; ref++) {
                    const uint8_t *pm = &sector[440u + pm_off];
                    uint8_t pm_len = pm[1];

                    if (pm_len < 6u || pm_off + pm_len > mt_len)
                        break;

                    if (pm[0] == 1) {
                        if (!have_part_number) {
                            part_number = udf_le16(&pm[4]);
                            have_part_number = 1;
                            ctx->phys_part_ref = (uint16_t)ref;
                        }
                    } else if (pm[0] == 2 && pm_len >= 64u) {
                        int is_meta = memcmp(&pm[5], "*UDF Metadata Partition",
                                             23) == 0;

                        if (is_meta || !have_part_number) {
                            part_number = udf_le16(&pm[38]);
                            have_part_number = 1;
                        }
                        if (is_meta) {
                            ctx->has_meta_part = 1;
                            ctx->meta_part_ref = (uint16_t)ref;
                            meta_file_lba = udf_le32(&pm[40]);
                            meta_mirror_lba = udf_le32(&pm[44]);
                        }
                    }
                    pm_off += pm_len;
                }
            }
            break;

        case UDF_TAG_TD:
            goto vds_done;

        default:
            break;
        }
    }
vds_done:

    if (n_pds == 0 || !found_lvd) {
        ODFS_ERROR(log, ODFS_SUB_UDF,
                    "incomplete VDS: PD=%d LVD=%d", n_pds > 0, found_lvd);
        odfs_free(ctx);
        return ODFS_ERR_BAD_FORMAT;
    }

    if (lv_block_size != UDF_BLOCK_SIZE) {
        ODFS_ERROR(log, ODFS_SUB_UDF,
                    "unsupported logical block size %" PRIu32, lv_block_size);
        odfs_free(ctx);
        return ODFS_ERR_UNSUPPORTED;
    }

    /* the PD the partition maps refer to, or without maps the first */
    for (int i = 0; i < n_pds; i++) {
        if (!have_part_number || pd_numbers[i] == part_number) {
            pick = i;
            break;
        }
    }
    if (pick < 0) {
        ODFS_ERROR(log, ODFS_SUB_UDF,
                    "no PD for partition %u", part_number);
        odfs_free(ctx);
        return ODFS_ERR_BAD_FORMAT;
    }
    ctx->part_number = pd_numbers[pick];
    ctx->part_start  = pd_starts[pick];
    ctx->part_length = pd_lengths[pick];

    if (ctx->has_meta_part) {
        err = udf_load_meta_file(ctx, cache, log,
                                 meta_file_lba, meta_mirror_lba);
        if (err != ODFS_OK) {
            odfs_free(ctx);
            return err;
        }
        ODFS_INFO(log, ODFS_SUB_UDF,
                   "metadata partition %u: %u Metadata File extents",
                   ctx->meta_part_ref, ctx->meta_ext_count);
    }

    /* read File Set Descriptor */
    uint32_t fsd_phys = 0;
    err = udf_part_to_phys(ctx, fsd_ad.partition, fsd_ad.lba,
                           &fsd_phys, NULL);
    if (err == ODFS_OK)
        err = odfs_cache_read(cache, fsd_phys, &sector);
    if (err != ODFS_OK) { odfs_free(ctx); return err; }

    if (udf_tag_id(sector) != UDF_TAG_FSD) {
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
    uint32_t root_phys = 0;
    err = udf_part_to_phys(ctx, ctx->root_icb_part, ctx->root_icb_lba,
                           &root_phys, NULL);
    if (err == ODFS_OK)
        err = odfs_cache_read(cache, root_phys, &sector);
    if (err != ODFS_OK) { odfs_free(ctx); return err; }

    tag = udf_tag_id(sector);
    if (tag != UDF_TAG_FE && tag != UDF_TAG_EFE) {
        ODFS_ERROR(log, ODFS_SUB_UDF,
                    "root ICB tag %" PRIu16 " (expected FE/EFE)", tag);
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

    /* the Modification Time is at 84 in a File Entry and at 92 in an
     * Extended File Entry, which has the Object Size before it */
    root_out->size = udf_le64(&sector[56]);
    udf_parse_timestamp(&sector[tag == UDF_TAG_FE ? 84 : 92],
                        &root_out->mtime);
    root_out->ctime = root_out->mtime;

    /* store extent as the ICB location (for readdir) */
    root_out->extent.lba    = root_phys;
    root_out->extent.length = (uint32_t)root_out->size;
    ctx->root = *root_out;

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
/* File Entry parsing                                                  */
/* ------------------------------------------------------------------ */

typedef struct udf_icb_info {
    uint64_t data_size;   /* Information Length */
    uint32_t ad_offset;   /* allocation descriptors' offset in the ICB */
    uint32_t ad_length;
    uint8_t  alloc_type;
    uint8_t  file_type;
} udf_icb_info_t;

/*
 * Parse the File Entry or Extended File Entry ICB at icb_phys_lba.
 * Optionally return its modification time and the cached ICB sector,
 * which stays valid only until the next cache access.
 */
static odfs_err_t udf_parse_icb(odfs_cache_t *cache,
                                uint32_t icb_phys_lba,
                                udf_icb_info_t *out,
                                odfs_timestamp_t *mtime,
                                const uint8_t **sector_out)
{
    const uint8_t *sector;
    uint16_t tag;
    uint32_t ad_base, ea_len, ad_length;
    odfs_err_t err;

    err = odfs_cache_read(cache, icb_phys_lba, &sector);
    if (err != ODFS_OK)
        return err;

    tag = udf_tag_id(sector);
    if (tag == 0)
        return ODFS_ERR_CORRUPT;

    /* File Entry (ECMA-167 14.9) and Extended File Entry (14.17) agree
     * up to the Information Length; the EFE has 40 more bytes before
     * the EA and AD lengths that precede the descriptors */
    if (tag == UDF_TAG_FE)
        ad_base = 176;
    else if (tag == UDF_TAG_EFE)
        ad_base = 216;
    else
        return ODFS_ERR_BAD_FORMAT;

    ea_len = udf_le32(&sector[ad_base - 8]);
    ad_length = udf_le32(&sector[ad_base - 4]);
    if (ea_len > UDF_BLOCK_SIZE - ad_base ||
        ad_length > UDF_BLOCK_SIZE - (ad_base + ea_len))
        return ODFS_ERR_CORRUPT;

    out->data_size  = udf_le64(&sector[56]);
    out->ad_offset  = ad_base + ea_len;
    out->ad_length  = ad_length;
    out->alloc_type = (uint8_t)(udf_le16(&sector[16 + 18]) & 0x07);
    out->file_type  = sector[16 + 11];

    if (mtime)
        udf_parse_timestamp(&sector[tag == UDF_TAG_FE ? 84 : 92], mtime);
    if (sector_out)
        *sector_out = sector;
    return ODFS_OK;
}

/* ------------------------------------------------------------------ */
/* allocation descriptor cursor                                        */
/* ------------------------------------------------------------------ */

/*
 * Walks the allocation descriptors of a File Entry, following AED
 * continuations, one extent per udf_ad_next() call. Descriptors are
 * copied into a small window and the window is refilled from the
 * cache: reading file data between two calls can evict the ICB or AED
 * sector they live in, so the cursor holds no pointer into the cache.
 */
typedef struct udf_ad_cursor {
    const udf_context_t *ctx;
    odfs_cache_t        *cache;
    uint32_t             desc_lba;  /* sector holding the current run */
    uint32_t             run_off;   /* offset of the run in that sector */
    uint32_t             run_len;
    uint32_t             pos;       /* next descriptor within the run */
    uint32_t             win_start; /* run offset of win[0] */
    uint32_t             etype;     /* current extent */
    uint32_t             elen;
    uint32_t             phys_lba;
    uint32_t             rem_len;   /* rest of a split recorded extent */
    uint32_t             rem_lba;
    uint16_t             rem_part;
    uint16_t             short_part; /* partition of short ADs */
    int                  guard;
    uint8_t              ad_size;
    uint8_t              win_len;
    uint8_t              win[64];
} udf_ad_cursor_t;

static odfs_err_t udf_ad_init(udf_ad_cursor_t *cur,
                              const udf_context_t *ctx,
                              odfs_cache_t *cache,
                              uint32_t icb_phys,
                              const udf_icb_info_t *icb)
{
    if (icb->alloc_type == UDF_ICB_ALLOC_SHORT)
        cur->ad_size = 8;
    else if (icb->alloc_type == UDF_ICB_ALLOC_LONG)
        cur->ad_size = 16;
    else
        return ODFS_ERR_UNSUPPORTED; /* extended ADs are not used on optical media */

    cur->ctx = ctx;
    cur->cache = cache;
    cur->desc_lba = icb_phys;
    cur->run_off = icb->ad_offset;
    cur->run_len = icb->ad_length;
    cur->pos = 0;
    cur->win_start = 0;
    cur->win_len = 0;
    cur->rem_len = 0;
    cur->short_part = udf_icb_part(ctx, icb_phys);
    cur->guard = 0;
    return ODFS_OK;
}

/*
 * Advance to the next extent. On ODFS_OK, cur->elen > 0 describes it
 * (cur->phys_lba is valid for recorded extents), or cur->elen == 0
 * marks the end of the descriptors. A recorded extent that is not
 * contiguous on the media is returned in contiguous pieces.
 */
static odfs_err_t udf_ad_next(udf_ad_cursor_t *cur)
{
    uint32_t etype, elen, elba, run;
    uint16_t epart;
    odfs_err_t err;

    if (cur->rem_len) {
        etype = UDF_EXT_RECORDED;
        elen = cur->rem_len;
        elba = cur->rem_lba;
        epart = cur->rem_part;
        cur->rem_len = 0;
        goto extent;
    }

    for (;;) {
        const uint8_t *ad;
        uint32_t len_field;

        if (cur->pos + cur->ad_size > cur->run_len) {
            cur->elen = 0; /* end of this run and no continuation */
            return ODFS_OK;
        }

        if (cur->pos - cur->win_start + cur->ad_size > cur->win_len) {
            const uint8_t *sec;
            uint32_t wlen = cur->run_len - cur->pos;

            err = odfs_cache_read(cur->cache, cur->desc_lba, &sec);
            if (err != ODFS_OK)
                return err;
            if (wlen > sizeof(cur->win))
                wlen = sizeof(cur->win);
            memcpy(cur->win, sec + cur->run_off + cur->pos, wlen);
            cur->win_start = cur->pos;
            cur->win_len = (uint8_t)wlen;
        }

        ad = &cur->win[cur->pos - cur->win_start];
        len_field = udf_le32(&ad[0]);
        etype = len_field >> 30;
        elen  = len_field & UDF_EXT_LENGTH_MASK;
        elba  = udf_le32(&ad[4]); /* LBA field, same offset for short/long */
        epart = (cur->ad_size == 16) ? udf_le16(&ad[8]) : cur->short_part;

        if (etype == UDF_EXT_CONTINUATION) {
            const uint8_t *aed;
            uint32_t aed_lba;
            uint32_t alen;

            if (++cur->guard > 4096)
                return ODFS_ERR_CORRUPT; /* runaway AED chain */

            err = udf_part_to_phys(cur->ctx, epart, elba, &aed_lba, NULL);
            if (err != ODFS_OK)
                return err;
            err = odfs_cache_read(cur->cache, aed_lba, &aed);
            if (err != ODFS_OK)
                return err;
            if (udf_tag_id(aed) != UDF_TAG_AED)
                return ODFS_ERR_CORRUPT;

            alen = udf_le32(&aed[20]); /* Length of Allocation Descriptors */
            if (alen > UDF_BLOCK_SIZE - 24u)
                return ODFS_ERR_CORRUPT;

            cur->desc_lba = aed_lba;
            cur->run_off = 24;
            cur->run_len = alen;
            cur->pos = 0;
            cur->win_start = 0;
            cur->win_len = 0;
            continue;
        }

        cur->pos += cur->ad_size;
        break;
    }

extent:
    cur->etype = etype;
    cur->elen = elen; /* 0: terminating descriptor */
    cur->phys_lba = 0;
    if (elen == 0 || etype != UDF_EXT_RECORDED)
        return ODFS_OK;

    err = udf_part_to_phys(cur->ctx, epart, elba, &cur->phys_lba, &run);
    if (err != ODFS_OK)
        return err;
    if (run < (elen + UDF_BLOCK_SIZE - 1) / UDF_BLOCK_SIZE) {
        cur->elen = run * UDF_BLOCK_SIZE;
        cur->rem_len = elen - cur->elen;
        cur->rem_lba = elba + run;
        cur->rem_part = epart;
    }
    return ODFS_OK;
}

/*
 * Read up to *len bytes at file_offset from the object (file or directory)
 * whose File Entry ICB at icb_phys parses to *icb. This follows the full
 * allocation-descriptor chain, so files and directories that span
 * several extents — the normal case for anything larger than ~1 GiB, and
 * for fragmented media — are read correctly. Embedded data, sparse and
 * unrecorded extents (read as zeros), and allocation-extent continuation
 * (AED) blocks are all handled. On return *len holds the byte count
 * produced, which is short at end-of-file.
 */
static odfs_err_t udf_read_parsed(udf_context_t *ctx,
                                  odfs_cache_t *cache,
                                  uint32_t icb_phys,
                                  const udf_icb_info_t *icb,
                                  uint64_t file_offset,
                                  void *buf,
                                  size_t *len)
{
    udf_ad_cursor_t cur;
    uint8_t *out = buf;
    size_t want = *len;
    size_t done = 0;
    uint64_t cur_pos = 0;
    odfs_err_t err;

    *len = 0;

    if (file_offset >= icb->data_size)
        return ODFS_OK;
    if (want > icb->data_size - file_offset)
        want = (size_t)(icb->data_size - file_offset);
    if (want == 0)
        return ODFS_OK;

    if (icb->alloc_type == UDF_ICB_ALLOC_EMBEDDED) {
        if ((uint64_t)icb->ad_offset + icb->data_size > UDF_BLOCK_SIZE)
            return ODFS_ERR_CORRUPT;
        *len = want;
        return odfs_cache_read_bytes(cache, icb_phys,
                                     icb->ad_offset + (uint32_t)file_offset,
                                     out, len);
    }

    err = udf_ad_init(&cur, ctx, cache, icb_phys, icb);
    if (err != ODFS_OK)
        return err;

    while (done < want) {
        uint64_t ext_end, start_in_file;

        err = udf_ad_next(&cur);
        if (err != ODFS_OK) {
            *len = done;
            return err;
        }
        if (cur.elen == 0)
            break;

        ext_end = cur_pos + cur.elen;
        start_in_file = file_offset + done;

        if (start_in_file < ext_end) {
            uint32_t skip = (uint32_t)(start_in_file - cur_pos);
            size_t chunk = (size_t)(ext_end - start_in_file);

            if (chunk > want - done)
                chunk = want - done;

            if (cur.etype == UDF_EXT_RECORDED) {
                size_t n = chunk;
                err = odfs_cache_read_bytes(cache, cur.phys_lba,
                                            skip, out + done, &n);
                if (err != ODFS_OK) {
                    *len = done;
                    return err;
                }
                done += n;
                if (n < chunk) {
                    *len = done; /* short device read */
                    return ODFS_OK;
                }
            } else {
                /* allocated-but-unrecorded or unallocated: reads as zeros */
                memset(out + done, 0, chunk);
                done += chunk;
            }
        }

        cur_pos = ext_end;
    }

    /*
     * The descriptors may end before the Information Length; the rest
     * of a file is unallocated and reads as zeros. A directory that
     * ends early is truncated, and its short read reports that.
     */
    if (done < want && icb->file_type != UDF_ICB_FILETYPE_DIR) {
        memset(out + done, 0, want - done);
        done = want;
    }

    *len = done;
    return ODFS_OK;
}

/* Read exactly len bytes at offset; ODFS_ERR_EOF if fewer are available. */
static odfs_err_t udf_read_dir_exact(udf_context_t *ctx,
                                     odfs_cache_t *cache,
                                     uint32_t icb_phys,
                                     const udf_icb_info_t *icb,
                                     uint32_t offset,
                                     void *buf,
                                     size_t len)
{
    size_t got = len;
    odfs_err_t err = udf_read_parsed(ctx, cache, icb_phys, icb,
                                     offset, buf, &got);

    if (err != ODFS_OK)
        return err;
    if (got != len)
        return ODFS_ERR_EOF;
    return ODFS_OK;
}

/*
 * Load the extent list of the Metadata File, or failing that of its
 * mirror, which maps the metadata partition onto the physical one. The
 * Metadata File's own File Entry and extents are in the physical
 * partition. Unrecorded extents still occupy metadata partition blocks
 * and are kept as holes.
 */
static odfs_err_t udf_load_meta_file(udf_context_t *ctx,
                                     odfs_cache_t *cache,
                                     odfs_log_state_t *log,
                                     uint32_t file_lba,
                                     uint32_t mirror_lba)
{
    uint32_t cands[2] = { file_lba, mirror_lba };
    odfs_err_t err = ODFS_ERR_BAD_FORMAT;

    ctx->has_meta_part = 0; /* translate the descriptors as physical */

    for (int m = 0; m < 2; m++) {
        uint32_t icb_phys = ctx->part_start + cands[m];
        udf_icb_info_t icb;
        udf_ad_cursor_t cur;

        if (cands[m] == 0xFFFFFFFFu || (m == 1 && cands[1] == cands[0]))
            continue;

        ctx->meta_ext_count = 0;
        err = udf_parse_icb(cache, icb_phys, &icb, NULL, NULL);
        if (err == ODFS_OK)
            err = udf_ad_init(&cur, ctx, cache, icb_phys, &icb);

        while (err == ODFS_OK) {
            udf_meta_extent_t *ext;

            err = udf_ad_next(&cur);
            if (err != ODFS_OK || cur.elen == 0)
                break;
            if (ctx->meta_ext_count == UDF_MAX_META_EXTENTS) {
                ODFS_ERROR(log, ODFS_SUB_UDF,
                            "more than %u Metadata File extents",
                            UDF_MAX_META_EXTENTS);
                err = ODFS_ERR_UNSUPPORTED;
                break;
            }
            ext = &ctx->meta_extents[ctx->meta_ext_count++];
            ext->blocks = (cur.elen + UDF_BLOCK_SIZE - 1) / UDF_BLOCK_SIZE;
            ext->phys_lba = (cur.etype == UDF_EXT_RECORDED)
                            ? cur.phys_lba : UDF_META_HOLE;
        }

        if (err == ODFS_OK && ctx->meta_ext_count == 0)
            err = ODFS_ERR_BAD_FORMAT;
        if (err == ODFS_OK) {
            ctx->has_meta_part = 1;
            return ODFS_OK;
        }
        ODFS_ERROR(log, ODFS_SUB_UDF,
                    "Metadata%s File at part LBA %" PRIu32 ": %s",
                    m ? " Mirror" : "", cands[m], odfs_err_str(err));
    }

    return err;
}

static odfs_err_t udf_fill_node_from_icb(udf_context_t *ctx,
                                         odfs_cache_t *cache,
                                         uint8_t fid_flags,
                                         uint16_t icb_part,
                                         uint32_t icb_lba,
                                         odfs_node_t *node)
{
    uint32_t icb_phys;
    udf_icb_info_t icb;
    odfs_err_t err;

    err = udf_part_to_phys(ctx, icb_part, icb_lba, &icb_phys, NULL);
    if (err != ODFS_OK)
        return err;

    err = udf_parse_icb(cache, icb_phys, &icb, &node->mtime, NULL);
    if (err != ODFS_OK)
        return err;

    node->size = icb.data_size;
    node->ctime = node->mtime;
    node->extent.lba = icb_phys;
    node->extent.length = (uint32_t)icb.data_size;

    if (fid_flags & UDF_FID_FLAG_DIRECTORY)
        node->kind = ODFS_NODE_DIR;
    else if (icb.file_type == UDF_ICB_FILETYPE_SYMLINK)
        node->kind = ODFS_NODE_SYMLINK;
    else
        node->kind = ODFS_NODE_FILE;

    return ODFS_OK;
}

/*
 * A child whose File Entry cannot be parsed is skipped, but media and
 * allocation failures must reach the caller: skipping on those would
 * report an existing file as missing, and a readdir snapshot taken by
 * the metadata cache would keep it missing for the rest of the mount.
 */
static inline int udf_is_fatal_err(odfs_err_t err)
{
    return err == ODFS_ERR_IO || err == ODFS_ERR_NOMEM;
}

typedef struct udf_fid_info {
    const uint8_t *name;     /* CS0 name, name_len bytes */
    uint32_t       start;
    uint32_t       next;
    uint32_t       length;
    uint32_t       icb_lba;
    uint16_t       icb_part;
    uint16_t       impl_len;
    uint8_t        flags;
    uint8_t        name_len;
} udf_fid_info_t;

typedef odfs_err_t (*udf_fid_fn)(const udf_fid_info_t *fid, void *arg);

/*
 * Position in a directory's FID stream. The current directory sector
 * is reused for consecutive FIDs as long as the cache generation shows
 * that it has not been replaced in between.
 */
typedef struct udf_dir_stream {
    udf_context_t  *ctx;
    odfs_cache_t   *cache;
    const uint8_t  *sec;
    uint32_t        icb_phys;
    uint32_t        sec_lba;
    uint32_t        sec_gen;
    uint32_t        ext_start;  /* stream offset of the current extent */
    uint32_t        ext_len;
    uint32_t        ext_phys;
    udf_icb_info_t  icb;
    udf_ad_cursor_t cur;
} udf_dir_stream_t;

/*
 * Return the 38-byte FID header at stream offset raw_pos (directory
 * offset log_pos): a pointer into the cached sector, with the number
 * of contiguous bytes available there, or a copy in scratch when the
 * header straddles a sector or extent boundary.
 */
static odfs_err_t udf_stream_hdr(udf_dir_stream_t *st,
                                 uint32_t raw_pos,
                                 uint32_t log_pos,
                                 uint8_t *scratch,
                                 const uint8_t **out,
                                 uint32_t *avail_out)
{
    uint32_t in_ext, lba, off, av_sec, av_ext, avail;
    odfs_err_t err;

    while (raw_pos - st->ext_start >= st->ext_len) {
        st->ext_start += st->ext_len;
        st->ext_len = 0;
        err = udf_ad_next(&st->cur);
        if (err != ODFS_OK)
            return err;
        if (st->cur.elen == 0)
            return ODFS_ERR_EOF;
        if (st->cur.etype != UDF_EXT_RECORDED)
            return ODFS_ERR_CORRUPT;
        st->ext_len = (st->cur.elen > UINT32_MAX - st->ext_start)
                      ? (UINT32_MAX - st->ext_start) : st->cur.elen;
        st->ext_phys = st->cur.phys_lba;
    }

    in_ext = raw_pos - st->ext_start;
    lba = st->ext_phys + in_ext / UDF_BLOCK_SIZE;
    off = in_ext % UDF_BLOCK_SIZE;
    av_sec = UDF_BLOCK_SIZE - off;
    av_ext = st->ext_len - in_ext;
    avail = (av_sec < av_ext) ? av_sec : av_ext;

    if (avail >= 38u) {
        if (!st->sec || st->sec_lba != lba ||
            st->cache->generation != st->sec_gen) {
            err = odfs_cache_read(st->cache, lba, &st->sec);
            if (err != ODFS_OK)
                return err;
            st->sec_lba = lba;
            st->sec_gen = st->cache->generation;
        }
        *out = st->sec + off;
        *avail_out = avail;
        return ODFS_OK;
    }

    err = udf_read_dir_exact(st->ctx, st->cache, st->icb_phys, &st->icb,
                             log_pos, scratch, 38u);
    if (err != ODFS_OK)
        return err;
    *out = scratch;
    *avail_out = 38u;
    return ODFS_OK;
}

static odfs_err_t udf_walk_fids(udf_context_t *ctx,
                                odfs_cache_t *cache,
                                const odfs_node_t *dir,
                                udf_fid_fn callback,
                                void *arg,
                                uint32_t *dir_size_out)
{
    udf_dir_stream_t st;
    uint8_t fid_buf[320]; /* header + impl use + name, when straddling */
    uint32_t dir_size, bias = 0, offset = 0;
    odfs_err_t err;

    st.ctx = ctx;
    st.cache = cache;
    st.sec = NULL;
    st.icb_phys = dir->extent.lba;
    st.sec_lba = 0;
    st.sec_gen = 0;
    st.ext_start = 0;
    st.ext_len = 0;
    st.ext_phys = 0;

    err = udf_parse_icb(cache, st.icb_phys, &st.icb, NULL, &st.sec);
    if (err != ODFS_OK)
        return err;
    if (st.icb.data_size > UINT32_MAX)
        return ODFS_ERR_CORRUPT;

    /* a directory read must stop at the recorded data, not zero-fill */
    st.icb.file_type = UDF_ICB_FILETYPE_DIR;
    dir_size = (uint32_t)st.icb.data_size;
    if (dir_size_out)
        *dir_size_out = dir_size;

    if (st.icb.alloc_type == UDF_ICB_ALLOC_EMBEDDED) {
        /* the FIDs follow the EAs inside the ICB sector itself */
        if ((uint64_t)st.icb.ad_offset + dir_size > UDF_BLOCK_SIZE)
            return ODFS_ERR_CORRUPT;
        bias = st.icb.ad_offset;
        st.ext_len = bias + dir_size;
        st.ext_phys = st.icb_phys;
        st.sec_lba = st.icb_phys;
        st.sec_gen = cache->generation;
    } else if (dir_size > 0) {
        err = udf_ad_init(&st.cur, ctx, cache, st.icb_phys, &st.icb);
        if (err != ODFS_OK)
            return err;
        st.sec = NULL;
    }

    while (offset < dir_size) {
        uint32_t remaining = dir_size - offset;
        const uint8_t *hdr;
        uint32_t avail;
        udf_fid_info_t fid;

        if (remaining < 38u)
            break;

        err = udf_stream_hdr(&st, bias + offset, offset, fid_buf,
                             &hdr, &avail);
        if (err != ODFS_OK)
            return err;

        if (udf_tag_id(hdr) != UDF_TAG_FID)
            break;

        fid.start = offset;
        fid.flags = hdr[18];
        fid.name_len = hdr[19];
        fid.icb_lba = udf_le32(&hdr[20 + 4]);
        fid.icb_part = udf_le16(&hdr[20 + 8]);
        fid.impl_len = udf_le16(&hdr[36]);
        fid.length =
            (38u + fid.impl_len + fid.name_len + 3u) & ~3u;

        if (fid.length > remaining)
            return ODFS_ERR_CORRUPT;

        if (fid.length <= avail) {
            fid.name = hdr + 38u + fid.impl_len;
        } else {
            /*
             * The FID continues past this sector or extent. Fetch the
             * name for live entries; deleted and parent FIDs have no
             * name we use, but read their last byte anyway, so that a
             * FID cut off by the end of the recorded data is still
             * reported as a truncated directory.
             */
            int need_name = !(fid.flags & (UDF_FID_FLAG_DELETED |
                                           UDF_FID_FLAG_PARENT)) &&
                            fid.name_len > 0;
            uint32_t body = 38u + fid.impl_len;
            uint32_t tail_off = need_name ? offset + body
                                          : offset + fid.length - 1u;
            size_t tail_len = need_name ? (size_t)(fid.length - body) : 1u;

            err = udf_read_dir_exact(ctx, cache, st.icb_phys, &st.icb,
                                     tail_off, fid_buf + 38u, tail_len);
            if (err != ODFS_OK)
                return err;
            fid.name = fid_buf + 38u;
        }

        fid.next = offset + fid.length;
        err = callback(&fid, arg);
        if (err != ODFS_OK)
            return err;
        offset = fid.next;
    }

    return ODFS_OK;
}

typedef struct udf_dir_entry {
    odfs_node_t node;
    uint32_t    start;
    uint32_t    next;
    uint32_t    icb_lba;
    uint16_t    icb_part;
    uint8_t     flags;
} udf_dir_entry_t;

typedef odfs_err_t (*udf_dir_entry_fn)(udf_dir_entry_t *entry, void *arg);

typedef struct udf_dir_walk_ctx {
    udf_context_t       *ctx;
    odfs_cache_t        *cache;
    const odfs_node_t   *dir;
    udf_dir_entry_fn     callback;
    void                *arg;
    odfs_namefix_state_t namefix;
} udf_dir_walk_ctx_t;

static odfs_err_t udf_decode_dir_entry(const udf_fid_info_t *fid, void *arg)
{
    udf_dir_walk_ctx_t *walk = arg;
    udf_dir_entry_t entry;
    odfs_err_t err;

    if (fid->flags & (UDF_FID_FLAG_DELETED | UDF_FID_FLAG_PARENT))
        return ODFS_OK;

    /* field by field: a memset would clear the whole name buffer for
     * every FID; the amiga_as payload is unused while its flags are 0 */
    entry.node.id = fid->icb_lba + 1u; /* 0 is the root */
    entry.node.parent_id = walk->dir->id;
    entry.node.backend = ODFS_BACKEND_UDF;
    entry.node.kind = ODFS_NODE_FILE;
    entry.node.name[0] = '\0';
    entry.node.size = 0;
    memset(&entry.node.mtime, 0, sizeof(entry.node.mtime));
    entry.node.ctime = entry.node.mtime;
    entry.node.mode = 0;
    entry.node.amiga_as.has_protection = 0;
    entry.node.amiga_as.has_comment = 0;
    entry.node.extent.lba = 0;
    entry.node.extent.length = 0;
    entry.node.backend_data = NULL;
    entry.start = fid->start;
    entry.next = fid->next;
    entry.icb_lba = fid->icb_lba;
    entry.icb_part = fid->icb_part;
    entry.flags = fid->flags;

    if (fid->name_len > 0)
        udf_decode_cs0(fid->name, fid->name_len,
                       entry.node.name, sizeof(entry.node.name));

    err = odfs_namefix_apply(&walk->namefix, entry.node.name,
                             sizeof(entry.node.name));
    if (err != ODFS_OK)
        return err;

    return walk->callback(&entry, walk->arg);
}

static odfs_err_t udf_walk_dir_entries(udf_context_t *ctx,
                                       odfs_cache_t *cache,
                                       const odfs_node_t *dir,
                                       udf_dir_entry_fn callback,
                                       void *arg,
                                       uint32_t *dir_size_out)
{
    udf_dir_walk_ctx_t walk;
    odfs_err_t err;

    walk.ctx = ctx;
    walk.cache = cache;
    walk.dir = dir;
    walk.callback = callback;
    walk.arg = arg;
    odfs_namefix_init(&walk.namefix, cache->name_charset);
    err = udf_walk_fids(ctx, cache, dir, udf_decode_dir_entry, &walk,
                        dir_size_out);
    odfs_namefix_destroy(&walk.namefix);
    return err;
}

/* ------------------------------------------------------------------ */
/* readdir                                                             */
/* ------------------------------------------------------------------ */

typedef struct udf_readdir_ctx {
    udf_context_t  *ctx;
    odfs_cache_t   *cache;
    odfs_dir_iter_fn callback;
    void           *arg;
    uint32_t        target;
    uint32_t       *resume;
} udf_readdir_ctx_t;

static odfs_err_t udf_readdir_entry(udf_dir_entry_t *entry, void *arg)
{
    udf_readdir_ctx_t *walk = arg;
    odfs_err_t err;

    if (entry->start < walk->target)
        return ODFS_OK;

    err = udf_fill_node_from_icb(walk->ctx, walk->cache,
                                 entry->flags, entry->icb_part,
                                 entry->icb_lba, &entry->node);
    if (err != ODFS_OK)
        return udf_is_fatal_err(err) ? err : ODFS_OK;

    err = walk->callback(&entry->node, walk->arg);
    if (err != ODFS_OK && walk->resume)
        *walk->resume = entry->next;
    return err;
}

static odfs_err_t udf_readdir(void *backend_ctx,
                                odfs_cache_t *cache,
                                odfs_log_state_t *log,
                                const odfs_node_t *dir,
                                odfs_dir_iter_fn callback,
                                void *cb_ctx,
                                uint32_t *resume_offset)
{
    udf_context_t *ctx = backend_ctx;
    udf_readdir_ctx_t walk;
    uint32_t dir_size;
    odfs_err_t err;

    (void)log;

    walk.ctx = ctx;
    walk.cache = cache;
    walk.callback = callback;
    walk.arg = cb_ctx;
    walk.target =
        (resume_offset && *resume_offset) ? *resume_offset : 0;
    walk.resume = resume_offset;

    err = udf_walk_dir_entries(ctx, cache, dir, udf_readdir_entry,
                               &walk, &dir_size);
    if (err != ODFS_OK)
        return err;

    if (resume_offset)
        *resume_offset = dir_size;
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
    udf_icb_info_t icb;
    odfs_err_t err;

    (void)log;

    err = udf_parse_icb(cache, file->extent.lba, &icb, NULL, NULL);
    if (err != ODFS_OK) {
        *len = 0;
        return err;
    }

    /* the object walker handles embedded data, sparse extents, and
     * allocation-descriptor chains spanning AED continuation blocks */
    return udf_read_parsed(ctx, cache, file->extent.lba, &icb,
                           offset, buf, len);
}

/* ------------------------------------------------------------------ */
/* lookup                                                              */
/* ------------------------------------------------------------------ */

typedef struct udf_match_ctx {
    udf_context_t *ctx;
    odfs_cache_t  *cache;
    const char    *name;      /* match by name, or if NULL ... */
    uint32_t       child_icb; /* ... the directory with this ICB */
    odfs_node_t   *out;
    int            found;
} udf_match_ctx_t;

static odfs_err_t udf_match_entry(udf_dir_entry_t *entry, void *arg)
{
    udf_match_ctx_t *m = arg;
    uint32_t phys;
    odfs_err_t err;

    if (m->name) {
        if (odfs_strcasecmp(entry->node.name, m->name) != 0)
            return ODFS_OK;
    } else if (udf_part_to_phys(m->ctx, entry->icb_part, entry->icb_lba,
                                &phys, NULL) != ODFS_OK ||
               phys != m->child_icb) {
        return ODFS_OK;
    }

    err = udf_fill_node_from_icb(m->ctx, m->cache,
                                 entry->flags, entry->icb_part,
                                 entry->icb_lba, &entry->node);
    if (err != ODFS_OK)
        return udf_is_fatal_err(err) ? err : ODFS_OK;
    if (!m->name && entry->node.kind != ODFS_NODE_DIR)
        return ODFS_OK;

    *m->out = entry->node;
    m->found = 1;
    return ODFS_ERR_EOF;
}

static __attribute__((noinline)) odfs_err_t
udf_find_entry(udf_context_t *ctx,
               odfs_cache_t *cache,
               const odfs_node_t *dir,
               const char *name,
               uint32_t child_icb,
               odfs_node_t *out)
{
    udf_match_ctx_t m;
    odfs_err_t err;

    m.ctx = ctx;
    m.cache = cache;
    m.name = name;
    m.child_icb = child_icb;
    m.out = out;
    m.found = 0;

    err = udf_walk_dir_entries(ctx, cache, dir, udf_match_entry, &m, NULL);
    if (m.found)
        return ODFS_OK;
    if (err != ODFS_OK)
        return err;
    return ODFS_ERR_NOT_FOUND;
}

static odfs_err_t udf_lookup(void *backend_ctx,
                             odfs_cache_t *cache,
                             odfs_log_state_t *log,
                             const odfs_node_t *dir,
                             const char *name,
                             odfs_node_t *out)
{
    (void)log;

    return udf_find_entry(backend_ctx, cache, dir, name, 0, out);
}

/* ------------------------------------------------------------------ */
/* readlink                                                            */
/* ------------------------------------------------------------------ */

/*
 * A UDF symlink's file data is a sequence of Path Component records
 * (ECMA-167 4/14.16.1): componentType (1), identifier length (1),
 * file version (2), identifier in OSTA CS0. Decode them into a
 * POSIX-style path.
 */
static odfs_err_t udf_readlink(void *backend_ctx,
                               odfs_cache_t *cache,
                               odfs_log_state_t *log,
                               const odfs_node_t *dir,
                               const char *name,
                               char *buf,
                               size_t buf_size)
{
    udf_context_t *ctx = backend_ctx;
    odfs_node_t node;
    uint8_t data[512];
    size_t len;
    size_t pos = 0;
    size_t out = 0;
    odfs_err_t err;

    (void)ctx;

    err = udf_lookup(backend_ctx, cache, log, dir, name, &node);
    if (err != ODFS_OK)
        return err;
    if (node.kind != ODFS_NODE_SYMLINK)
        return ODFS_ERR_UNSUPPORTED;
    if (node.size == 0 || node.size > sizeof(data))
        return ODFS_ERR_UNSUPPORTED;

    len = (size_t)node.size;
    err = udf_read(backend_ctx, cache, log, &node, 0, data, &len);
    if (err != ODFS_OK)
        return err;

    /*
     * Some implementations (notably macOS hdiutil) store the target as a
     * raw path string instead of ECMA-167 path components. Component
     * types are 1..5, which no printable path byte can be, so the first
     * byte tells the two encodings apart.
     */
    if (len > 0 && data[0] > 5) {
        if (len >= buf_size)
            return ODFS_ERR_NAME_TOO_LONG;
        memcpy(buf, data, len);
        buf[len] = '\0';
        return ODFS_OK;
    }

    while (pos + 4 <= len) {
        uint8_t ctype = data[pos];
        uint8_t clen = data[pos + 1];
        char comp[256];
        size_t comp_len;

        if (pos + 4 + clen > len)
            break;

        switch (ctype) {
        case 1: /* path outside this volume's scope — treat as root */
        case 2: /* restart from the root: absolute path */
            out = 0;
            if (out + 1 >= buf_size)
                return ODFS_ERR_NAME_TOO_LONG;
            buf[out++] = '/';
            break;
        case 3: /* parent directory */
            comp[0] = '.';
            comp[1] = '.';
            comp[2] = '\0';
            goto append;
        case 4: /* current directory */
            comp[0] = '.';
            comp[1] = '\0';
            goto append;
        case 5: /* named component */
            udf_decode_cs0(&data[pos + 4], clen, comp, sizeof(comp));
            if (comp[0] == '\0')
                break;
        append:
            comp_len = strlen(comp);
            if (out > 0 && buf[out - 1] != '/') {
                if (out + 1 >= buf_size)
                    return ODFS_ERR_NAME_TOO_LONG;
                buf[out++] = '/';
            }
            if (out + comp_len >= buf_size)
                return ODFS_ERR_NAME_TOO_LONG;
            memcpy(buf + out, comp, comp_len);
            out += comp_len;
            break;
        default:
            break;
        }

        pos += 4u + clen;
    }

    if (out == 0)
        return ODFS_ERR_UNSUPPORTED;

    buf[out] = '\0';
    return ODFS_OK;
}

/* ------------------------------------------------------------------ */
/* resolve_parent                                                      */
/* ------------------------------------------------------------------ */

typedef struct udf_parent_fid_ctx {
    udf_context_t *ctx;
    uint32_t      *parent_icb;
    int            found;
} udf_parent_fid_ctx_t;

static odfs_err_t udf_parent_fid(const udf_fid_info_t *fid, void *arg)
{
    udf_parent_fid_ctx_t *parent = arg;

    if ((fid->flags & UDF_FID_FLAG_DELETED) == 0 &&
        (fid->flags & UDF_FID_FLAG_PARENT) != 0) {
        odfs_err_t err = udf_part_to_phys(parent->ctx, fid->icb_part,
                                          fid->icb_lba, parent->parent_icb,
                                          NULL);
        if (err != ODFS_OK)
            return err;
        parent->found = 1;
        return ODFS_ERR_EOF;
    }

    return ODFS_OK;
}

static odfs_err_t udf_read_parent_icb(udf_context_t *ctx,
                                      odfs_cache_t *cache,
                                      const odfs_node_t *dir,
                                      uint32_t *parent_icb_out)
{
    udf_parent_fid_ctx_t parent;
    odfs_err_t err;

    parent.ctx = ctx;
    parent.parent_icb = parent_icb_out;
    parent.found = 0;

    err = udf_walk_fids(ctx, cache, dir, udf_parent_fid, &parent, NULL);
    if (parent.found)
        return ODFS_OK;
    if (err != ODFS_OK)
        return err;
    return ODFS_ERR_UNSUPPORTED;
}

static odfs_node_t udf_dir_stub(uint32_t icb)
{
    odfs_node_t n;

    memset(&n, 0, sizeof(n));
    n.backend = ODFS_BACKEND_UDF;
    n.kind = ODFS_NODE_DIR;
    n.extent.lba = icb;
    return n;
}

static odfs_err_t udf_resolve_parent(void *backend_ctx,
                                     odfs_cache_t *cache,
                                     odfs_log_state_t *log,
                                     const odfs_node_t *dir,
                                     odfs_node_t *parent_out,
                                     odfs_node_t *grandparent_out)
{
    udf_context_t *ctx = backend_ctx;
    uint32_t root_icb = ctx->root.extent.lba;
    odfs_node_t *outs[2];
    odfs_node_t stub;
    uint32_t cur_icb;
    odfs_err_t err;

    (void)log;

    if (dir->kind != ODFS_NODE_DIR)
        return ODFS_ERR_NOT_DIR;

    if (dir->extent.lba == root_icb)
        return ODFS_ERR_NOT_FOUND;

    err = udf_read_parent_icb(ctx, cache, dir, &cur_icb);
    if (err != ODFS_OK)
        return err;

    if (cur_icb == dir->extent.lba)
        return ODFS_ERR_NOT_FOUND;

    /*
     * A FID names its directory only in that directory's parent: find
     * the parent's own parent through its ".." FID, then the parent's
     * entry there by ICB. Repeat one level up for the grandparent.
     */
    outs[0] = parent_out;
    outs[1] = grandparent_out;
    stub = udf_dir_stub(cur_icb);

    for (int step = 0; step < 2 && outs[step]; step++) {
        uint32_t up_icb;

        if (cur_icb == root_icb) {
            *outs[step] = ctx->root;
            if (step == 0 && grandparent_out)
                *grandparent_out = ctx->root;
            return ODFS_OK;
        }

        stub.extent.lba = cur_icb;
        err = udf_read_parent_icb(ctx, cache, &stub, &up_icb);
        if (err != ODFS_OK)
            return err;

        stub.extent.lba = up_icb;
        err = udf_find_entry(ctx, cache, &stub, NULL, cur_icb, outs[step]);
        if (err != ODFS_OK)
            return err;
        cur_icb = up_icb;
    }

    return ODFS_OK;
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
/* get_volume_size                                                     */
/* ------------------------------------------------------------------ */

static uint32_t udf_get_volume_size(void *backend_ctx)
{
    udf_context_t *ctx = backend_ctx;

    return ctx->part_length;
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
    .readlink        = udf_readlink,
    .resolve_parent  = udf_resolve_parent,
    .get_volume_name = udf_get_volume_name,
    .get_volume_size = udf_get_volume_size,
};
