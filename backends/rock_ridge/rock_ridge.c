/*
 * rock_ridge.c — Rock Ridge extension parser
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "rock_ridge.h"
#include "odfs/cache.h"
#include "iso9660/iso9660.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static inline uint16_t rr_sig(const uint8_t *p)
{
    return (uint16_t)(p[0] << 8) | p[1];
}

static inline uint32_t rr_read_le32(const uint8_t *p)
{
    return iso_read_le32(p);
}

/*
 * Parse a 7-byte ISO 9660 directory record timestamp (same as ISO DR).
 */
static void rr_parse_ts7(const uint8_t *d, odfs_timestamp_t *ts)
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
 * Parse a 17-byte ISO 9660 long-form timestamp.
 */
static void rr_parse_ts17(const uint8_t *d, odfs_timestamp_t *ts)
{
    /* ASCII digits: "YYYYMMDDHHMMSSCC" + tz byte */
    int year = 0, i;
    for (i = 0; i < 4; i++)
        year = year * 10 + (d[i] - '0');
    ts->year   = year;
    ts->month  = (d[4] - '0') * 10 + (d[5] - '0');
    ts->day    = (d[6] - '0') * 10 + (d[7] - '0');
    ts->hour   = (d[8] - '0') * 10 + (d[9] - '0');
    ts->minute = (d[10] - '0') * 10 + (d[11] - '0');
    ts->second = (d[12] - '0') * 10 + (d[13] - '0');
    ts->tz_offset = (int16_t)((int8_t)d[16]) * 15;
}

/* ------------------------------------------------------------------ */
/* detection                                                           */
/* ------------------------------------------------------------------ */

int rr_detect(const uint8_t *sua, size_t sua_len, int *skip_out)
{
    size_t pos = 0;

    *skip_out = 0;

    while (pos + 4 <= sua_len) {
        uint16_t sig = rr_sig(&sua[pos]);
        uint8_t len = sua[pos + 2];

        if (len < 4 || pos + len > sua_len)
            break;

        if (sig == RR_SIG_SP && len >= 7) {
            /* SP: sig(2) + len(1) + ver(1) + check1(1) + check2(1) + skip(1) */
            if (sua[pos + 4] == RR_SP_CHECK1 && sua[pos + 5] == RR_SP_CHECK2) {
                *skip_out = sua[pos + 6];
                return 1;
            }
        }
        pos += len;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* parsing                                                             */
/* ------------------------------------------------------------------ */

static void rr_parse_entries(const uint8_t *sua, size_t sua_len,
                             int skip, rr_info_t *info,
                             struct odfs_cache *cache, int depth);

static void rr_parse_entries(const uint8_t *sua, size_t sua_len,
                             int skip, rr_info_t *info,
                             struct odfs_cache *cache, int depth)
{
    size_t pos = (size_t)skip;
    size_t name_pos = info->has_name ? strlen(info->name) : 0;

    /* guard against infinite CE loops */
    if (depth > 8)
        return;

    while (pos + 4 <= sua_len) {
        uint16_t sig = rr_sig(&sua[pos]);
        uint8_t len = sua[pos + 2];

        if (len < 4 || pos + len > sua_len)
            break;

        switch (sig) {

        case RR_SIG_NM:
            if (len > 5) {
                uint8_t flags = sua[pos + 4];
                if (!(flags & RR_NM_CURRENT) && !(flags & RR_NM_PARENT)) {
                    size_t nlen = len - 5;
                    if (name_pos + nlen < sizeof(info->name) - 1) {
                        memcpy(info->name + name_pos, &sua[pos + 5], nlen);
                        name_pos += nlen;
                        info->name[name_pos] = '\0';
                    }
                    if (!(flags & RR_NM_CONTINUE))
                        info->has_name = 1;
                }
            }
            break;

        case RR_SIG_PX:
            if (len >= 36) {
                info->mode   = rr_read_le32(&sua[pos + 4]);
                info->nlinks = rr_read_le32(&sua[pos + 12]);
                info->uid    = rr_read_le32(&sua[pos + 20]);
                info->gid    = rr_read_le32(&sua[pos + 28]);
                info->has_posix = 1;
            }
            break;

        case RR_SIG_TF: {
            uint8_t flags = sua[pos + 4];
            const uint8_t *tp = &sua[pos + 5];
            int ts_size = (flags & RR_TF_LONG_FORM) ? 17 : 7;
            void (*parse_ts)(const uint8_t *, odfs_timestamp_t *) =
                (flags & RR_TF_LONG_FORM) ? rr_parse_ts17 : rr_parse_ts7;

            if (flags & RR_TF_CREATION) {
                if (tp + ts_size <= sua + pos + len) {
                    parse_ts(tp, &info->ctime);
                    info->has_timestamps = 1;
                }
                tp += ts_size;
            }
            if (flags & RR_TF_MODIFY) {
                if (tp + ts_size <= sua + pos + len) {
                    parse_ts(tp, &info->mtime);
                    info->has_timestamps = 1;
                }
                tp += ts_size;
            }
            /* skip remaining TF fields (access, attributes, etc.) */
            break;
        }

        case RR_SIG_SL:
            /* symbolic link — parse component records */
            if (len > 5) {
                size_t spos = 5;
                size_t sl_pos = strlen(info->symlink_target);
                while (spos + 2 <= len) {
                    uint8_t cflags = sua[pos + spos];
                    uint8_t clen = sua[pos + spos + 1];
                    if (cflags & 0x02) {
                        /* current directory "." */
                        if (sl_pos + 1 < sizeof(info->symlink_target))
                            info->symlink_target[sl_pos++] = '.';
                    } else if (cflags & 0x04) {
                        /* parent directory ".." */
                        if (sl_pos + 2 < sizeof(info->symlink_target)) {
                            info->symlink_target[sl_pos++] = '.';
                            info->symlink_target[sl_pos++] = '.';
                        }
                    } else if (cflags & 0x08) {
                        /* root "/" */
                        if (sl_pos < sizeof(info->symlink_target))
                            info->symlink_target[sl_pos++] = '/';
                    } else if (clen > 0) {
                        if (sl_pos > 0 && info->symlink_target[sl_pos - 1] != '/')
                            if (sl_pos < sizeof(info->symlink_target))
                                info->symlink_target[sl_pos++] = '/';
                        if (sl_pos + clen < sizeof(info->symlink_target)) {
                            memcpy(info->symlink_target + sl_pos,
                                   &sua[pos + spos + 2], clen);
                            sl_pos += clen;
                        }
                    }
                    info->symlink_target[sl_pos] = '\0';
                    spos += 2 + clen;
                }
                info->is_symlink = 1;
            }
            break;

        case RR_SIG_CL:
            if (len >= 12) {
                info->child_link_lba = rr_read_le32(&sua[pos + 4]);
                info->has_child_link = 1;
            }
            break;

        case RR_SIG_RE:
            info->is_relocated = 1;
            break;

        case RR_SIG_CE:
            /* continuation area: read from another sector */
            if (len >= 28 && cache) {
                uint32_t ce_lba  = rr_read_le32(&sua[pos + 4]);
                uint32_t ce_off  = rr_read_le32(&sua[pos + 12]);
                uint32_t ce_len  = rr_read_le32(&sua[pos + 20]);

                if (ce_len > 0 && ce_len <= 2048) {
                    const uint8_t *ce_sector;
                    if (odfs_cache_read(cache, ce_lba, &ce_sector) == ODFS_OK) {
                        if (ce_off + ce_len <= 2048) {
                            rr_parse_entries(ce_sector + ce_off, ce_len,
                                             0, info, cache, depth + 1);
                        }
                    }
                }
            }
            break;

        case RR_SIG_ST:
            /* terminator — stop parsing */
            return;

        default:
            /* unknown entry — skip */
            break;
        }

        pos += len;
    }
}

void rr_parse(const uint8_t *sua, size_t sua_len, int skip,
              rr_info_t *info,
              struct odfs_cache *cache)
{
    memset(info, 0, sizeof(*info));
    rr_parse_entries(sua, sua_len, skip, info, cache, 0);
}
