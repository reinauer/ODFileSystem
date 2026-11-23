/*
 * rock_ridge.c — Rock Ridge extension parser
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "rock_ridge.h"

#if ODFS_FEATURE_ROCK_RIDGE

#include "odfs/cache.h"
#include "iso9660/iso9660.h"

#include <string.h>

#define RR_SL_MAX_PAYLOAD_LEN 250U

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static inline uint16_t rr_sig(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static inline uint32_t rr_read_le32(const uint8_t *p)
{
    return iso_read_le32(p);
}

static void rr_parse_as(const uint8_t *entry, size_t entry_len, rr_info_t *info)
{
    uint8_t flags;
    size_t pos = 5;

    if (entry_len < 5)
        return;

    flags = entry[4];

    if (flags & RR_AS_PROTECTION) {
        if (pos + 4 > entry_len)
            return;
        if (!info->has_amiga_protection) {
            memcpy(info->amiga_protection, entry + pos, 4);
            info->has_amiga_protection = 1;
        }
        pos += 4;
    }

    if (flags & RR_AS_COMMENT) {
        size_t comment_pos;
        uint8_t frag_len;
        size_t copy_len;

        if (pos + 1 > entry_len)
            return;

        frag_len = entry[pos++];
        if (frag_len == 0 || pos + ((size_t)frag_len - 1) > entry_len)
            return;

        comment_pos = info->has_amiga_comment ? strlen(info->amiga_comment) : 0;
        if (comment_pos >= sizeof(info->amiga_comment))
            comment_pos = sizeof(info->amiga_comment) - 1;

        copy_len = (size_t)frag_len - 1;
        if (copy_len > (sizeof(info->amiga_comment) - 1) - comment_pos)
            copy_len = (sizeof(info->amiga_comment) - 1) - comment_pos;

        if (copy_len > 0) {
            memcpy(info->amiga_comment + comment_pos, entry + pos, copy_len);
            comment_pos += copy_len;
            info->amiga_comment[comment_pos] = '\0';
        }
        info->has_amiga_comment = 1;
    }
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
    ts->tz_offset = (int16_t)((int8_t)d[6] * 15);
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
    ts->month  = (uint8_t)((d[4] - '0') * 10 + (d[5] - '0'));
    ts->day    = (uint8_t)((d[6] - '0') * 10 + (d[7] - '0'));
    ts->hour   = (uint8_t)((d[8] - '0') * 10 + (d[9] - '0'));
    ts->minute = (uint8_t)((d[10] - '0') * 10 + (d[11] - '0'));
    ts->second = (uint8_t)((d[12] - '0') * 10 + (d[13] - '0'));
    ts->tz_offset = (int16_t)((int8_t)d[16] * 15);
}

/* ------------------------------------------------------------------ */
/* detection                                                           */
/* ------------------------------------------------------------------ */

int rr_detect(const uint8_t *sua, size_t sua_len, int *skip_out)
{
    size_t pos = 0;

    *skip_out = 0;

    /*
     * CD-ROM XA discs start every System Use area with a 14-byte XA
     * record ("XA" at bytes 6-7) that is not a SUSP entry. mkisofs -XA
     * then sets the SP skip length to 14 for all other records.
     */
    if (sua_len >= 14 && sua[6] == 'X' && sua[7] == 'A' &&
        rr_sig(sua) != RR_SIG_SP)
        pos = 14;

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

/* parser state carried from a System Use area into its continuations */
typedef struct rr_state {
    size_t   name_pos;  /* NM bytes collected so far */
    char    *sl_out;    /* SL target buffer, or NULL to skip decoding */
    size_t   sl_size;
    size_t   sl_pos;    /* SL target bytes written so far */
    uint8_t  sl_cont;   /* last SL component continues in the next one */
    unsigned blk_shift; /* log2(2048 / logical block size) */
    uint32_t ce_lba;    /* continuation area named by the current area */
    uint32_t ce_off;
    uint32_t ce_len;
} rr_state_t;

/* note the continuation area of CE entry e in *st; 0 if out of bounds */
static int rr_set_ce(const uint8_t *e, rr_state_t *st)
{
    uint32_t lba = rr_read_le32(&e[4]);
    uint32_t off = rr_read_le32(&e[12]);
    uint32_t len = rr_read_le32(&e[20]);

    if (off >= ISO_SECTOR_SIZE)
        return 0;
    if (st->blk_shift) {
        /* a smaller logical block may start inside a 2048-byte sector */
        uint32_t mask = (1u << st->blk_shift) - 1;

        off += (lba & mask) << (11 - st->blk_shift);
        lba >>= st->blk_shift;
    }
    if (off >= ISO_SECTOR_SIZE || len == 0 || len > ISO_SECTOR_SIZE - off)
        return 0;
    st->ce_lba = lba;
    st->ce_off = off;
    st->ce_len = len;
    return 1;
}

/* the continuation area noted in *st, or NULL if it cannot be read */
static const uint8_t *rr_ce_area(struct odfs_cache *cache,
                                 uint32_t session_start,
                                 const rr_state_t *st)
{
    const uint8_t *sector;

    if (odfs_cache_read(cache, st->ce_lba + session_start,
                        &sector) != ODFS_OK)
        return NULL;
    return sector + st->ce_off;
}

/*
 * Parse the entries of one System Use area or continuation area. Returns
 * 1 when the area names a valid continuation area, which the caller reads
 * only after this area is done: reading it may evict the cache block that
 * sua points into.
 */
static int rr_parse_entries(const uint8_t *sua, size_t sua_len,
                            size_t pos, rr_info_t *info, rr_state_t *st)
{
    size_t name_pos = st->name_pos;
    int has_ce = 0;

    while (pos + 4 <= sua_len) {
        uint16_t sig = rr_sig(&sua[pos]);
        uint8_t len = sua[pos + 2];
        size_t entry_len;

        if (len < 4 || pos + len > sua_len)
            break;
        entry_len = len;

        switch (sig) {

        case RR_SIG_NM:
            if (entry_len > 5) {
                uint8_t flags = sua[pos + 4];
                if (!(flags & RR_NM_CURRENT) && !(flags & RR_NM_PARENT)) {
                    size_t copy_len = entry_len - 5;
                    size_t name_space = (sizeof(info->name) - 1) - name_pos;
                    if (copy_len > name_space)
                        copy_len = name_space;
                    if (copy_len > 0) {
                        memcpy(info->name + name_pos, &sua[pos + 5], copy_len);
                        name_pos += copy_len;
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

        case RR_SIG_AS:
            rr_parse_as(&sua[pos], entry_len, info);
            break;

        case RR_SIG_TF: {
            uint8_t flags;
            const uint8_t *tp;
            size_t tf_rem;
            size_t ts_len;
            void (*parse_ts)(const uint8_t *, odfs_timestamp_t *);

            if (entry_len < 5)
                break;
            flags = sua[pos + 4];
            tp = &sua[pos + 5];
            tf_rem = entry_len - 5;
            ts_len = (flags & RR_TF_LONG_FORM) ? 17U : 7U;
            parse_ts = (flags & RR_TF_LONG_FORM) ? rr_parse_ts17 : rr_parse_ts7;

            if (flags & RR_TF_CREATION) {
                if (ts_len > tf_rem)
                    break;
                parse_ts(tp, &info->ctime);
                info->has_ctime = 1;
                tp += ts_len;
                tf_rem -= ts_len;
            }
            if (flags & RR_TF_MODIFY) {
                if (ts_len > tf_rem)
                    break;
                parse_ts(tp, &info->mtime);
                info->has_mtime = 1;
                tp += ts_len;
                tf_rem -= ts_len;
            }
            /* skip remaining TF fields (access, attributes, etc.) */
            break;
        }

        case RR_SIG_SL:
            /* symbolic link — decode component records into sl_out */
            if (entry_len > 5) {
                const uint8_t *comp = &sua[pos + 5];
                size_t comp_area_len = entry_len - 5;
                size_t comp_pos = 0;
                char *out = st->sl_out;
                size_t sl_pos = st->sl_pos;

                /*
                 * SUSP entry lengths are one byte, so an SL payload cannot
                 * exceed 255 - 5 bytes. Keep an explicit bound here so the
                 * component walk does not use an unchecked on-disc length as
                 * its loop boundary.
                 */
                if (comp_area_len > RR_SL_MAX_PAYLOAD_LEN)
                    break;

                info->is_symlink = 1;
                if (!out || info->sl_truncated)
                    break;

                while (comp_pos + 2 <= comp_area_len) {
                    uint8_t cflags = comp[comp_pos];
                    size_t comp_len = comp[comp_pos + 1];
                    size_t record_len = 2 + comp_len;
                    const char *src = (const char *)(comp + comp_pos + 2);
                    size_t src_len = comp_len;
                    size_t sep;

                    if (record_len > comp_area_len - comp_pos)
                        break;

                    /* a '/' separates components, except after a
                     * component continued in this record (CONTINUE) */
                    sep = (!st->sl_cont && sl_pos > 0 &&
                           out[sl_pos - 1] != '/') ? 1U : 0U;

                    if (cflags & 0x02) {
                        /* current directory "." */
                        src = "..";
                        src_len = 1;
                    } else if (cflags & 0x04) {
                        /* parent directory ".." */
                        src = "..";
                        src_len = 2;
                    } else if (cflags & 0x08) {
                        /* root "/" */
                        src = "/";
                        src_len = (sl_pos > 0 && out[sl_pos - 1] == '/') ? 0 : 1;
                        sep = 0;
                    } else if (src_len == 0) {
                        sep = 0;
                    }

                    if (sl_pos + sep + src_len >= st->sl_size) {
                        info->sl_truncated = 1;
                        break;
                    }
                    if (sep)
                        out[sl_pos++] = '/';
                    memcpy(out + sl_pos, src, src_len);
                    sl_pos += src_len;
                    out[sl_pos] = '\0';
                    st->sl_cont = cflags & 0x01;
                    comp_pos += record_len;
                }
                st->sl_pos = sl_pos;
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
            /* continuation area: followed once this area is done */
            if (len >= 28 && rr_set_ce(&sua[pos], st))
                has_ce = 1;
            break;

        case RR_SIG_ST:
            /* terminator — stop parsing this area */
            goto out;

        default:
            /* unknown entry — skip */
            break;
        }

        pos += len;
    }

out:
    st->name_pos = name_pos;
    return has_ce;
}

void rr_parse(const uint8_t *sua, size_t sua_len, int skip,
              uint32_t session_start, unsigned blk_shift, rr_info_t *info,
              char *symlink_out, size_t symlink_size,
              struct odfs_cache *cache)
{
    size_t pos = (size_t)skip;
    rr_state_t st;
    int hop;

    memset(info, 0, sizeof(*info));
    st.name_pos = 0;
    st.sl_out = (symlink_size > 0) ? symlink_out : NULL;
    st.sl_size = symlink_size;
    st.sl_pos = 0;
    st.sl_cont = 0;
    st.blk_shift = blk_shift;
    if (st.sl_out)
        st.sl_out[0] = '\0';

    /* follow at most 8 continuation areas, guarding against CE loops */
    for (hop = 0; ; hop++) {
        if (!rr_parse_entries(sua, sua_len, pos, info, &st) ||
            !cache || hop >= 8)
            break;
        sua = rr_ce_area(cache, session_start, &st);
        if (!sua)
            break;
        sua_len = st.ce_len;
        pos = 0;
    }
}

int rr_find_parent_link(const uint8_t *sua, size_t sua_len, int skip,
                        uint32_t session_start, unsigned blk_shift,
                        struct odfs_cache *cache, uint32_t *lba_out)
{
    size_t pos = (size_t)skip;
    rr_state_t st;
    int hop;

    st.blk_shift = blk_shift;

    for (hop = 0; ; hop++) {
        int has_ce = 0;

        while (pos + 4 <= sua_len) {
            uint16_t sig = rr_sig(&sua[pos]);
            uint8_t len = sua[pos + 2];

            if (len < 4 || pos + len > sua_len || sig == RR_SIG_ST)
                break;
            if (sig == RR_SIG_PL && len >= 12) {
                *lba_out = rr_read_le32(&sua[pos + 4]);
                return 1;
            }
            if (sig == RR_SIG_CE && len >= 28 && rr_set_ce(&sua[pos], &st))
                has_ce = 1;
            pos += len;
        }

        if (!has_ce || !cache || hop >= 8)
            return 0;
        sua = rr_ce_area(cache, session_start, &st);
        if (!sua)
            return 0;
        sua_len = st.ce_len;
        pos = 0;
    }
}

#endif /* ODFS_FEATURE_ROCK_RIDGE */
