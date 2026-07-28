/*
 * cdda.c — CDDA virtual file backend
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Presents audio CD tracks as virtual WAV or AIFF files. The file
 * header is synthesized on-the-fly; audio data is read via the media
 * layer (SCSI Read CD on Amiga hardware).
 *
 * On mixed-mode discs (data + audio), tracks appear in a virtual
 * CDDA/ subdirectory so they don't mix with data files. On pure
 * audio CDs, tracks appear at the root.
 *
 * Track detection requires a TOC from the media layer. On host
 * images without TOC support, the backend will not activate.
 */

#include "cdda.h"
#include "odfs/alloc.h"
#include "odfs/cache.h"
#include "odfs/log.h"
#include "odfs/error.h"
#include "odfs/printf.h"
#include "odfs/string.h"

#include <string.h>
#include <inttypes.h>
#include <stdarg.h>

#define CDDA_CDDB_NAME "CDDB.txt"
#define CDDA_CDDB_NODE_ID 0x43444442u
#define CDDA_CDDB_LBA 0xfffffffeu
#define CDDA_CDTEXT_NAME "CD-TEXT.txt"
#define CDDA_CDTEXT_NODE_ID 0x43445458u
#define CDDA_CDTEXT_LBA 0xfffffffdu
#define CDDA_DISK_ICON_NAME "Disk.info"
#define CDDA_DISK_ICON_NODE_ID 0x44494e46u
#define CDDA_DISK_ICON_LBA 0xfffffffcu

/* ------------------------------------------------------------------ */
/* Header generation                                                   */
/* ------------------------------------------------------------------ */

static void cdda_write_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
}

static void cdda_write_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void cdda_write_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v);
}

static void cdda_write_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v);
}

static uint32_t cdda_header_size(cdda_file_format_t format)
{
    return (format == CDDA_FILE_FORMAT_AIFF)
        ? CDDA_AIFF_HEADER_SIZE
        : CDDA_WAV_HEADER_SIZE;
}

static uint32_t cdda_sum_digits(uint32_t value)
{
    uint32_t sum = 0;

    do {
        sum += value % 10u;
        value /= 10u;
    } while (value != 0);

    return sum;
}

static uint32_t cdda_track_offset_frames(const cdda_track_t *track)
{
    return track->start_lba + 150u;
}

static uint32_t cdda_total_seconds(const cdda_context_t *ctx)
{
    uint32_t first_frames;
    uint32_t leadout_frames;
    const cdda_track_t *last;

    if (ctx->track_count == 0)
        return 0;

    first_frames = cdda_track_offset_frames(&ctx->tracks[0]);
    last = &ctx->tracks[ctx->track_count - 1];
    leadout_frames = cdda_track_offset_frames(last) + last->length_frames;
    return (leadout_frames / CDDA_FRAMES_PER_SEC) -
           (first_frames / CDDA_FRAMES_PER_SEC);
}

static uint32_t cdda_disc_id(const cdda_context_t *ctx)
{
    uint32_t sum = 0;

    for (int i = 0; i < ctx->track_count; i++)
        sum += cdda_sum_digits(cdda_track_offset_frames(&ctx->tracks[i]) /
                               CDDA_FRAMES_PER_SEC);

    return ((sum % 255u) << 24) |
           (cdda_total_seconds(ctx) << 8) |
           (uint32_t)ctx->track_count;
}

static void cdda_format_hex32(char *buf, uint32_t value)
{
    static const char hex[] = "0123456789abcdef";

    for (int i = 7; i >= 0; i--) {
        buf[i] = hex[value & 0x0fu];
        value >>= 4;
    }
    buf[8] = '\0';
}

static void cdda_set_volume_name(cdda_context_t *ctx)
{
    if (ctx->is_mixed_mode) {
        memcpy(ctx->volume_name, "CDDA", 5);
        return;
    }

    /*
     * Keep the pure-audio label simple so it survives the Amiga DOS
     * volume-node path unchanged.
     */
    memcpy(ctx->volume_name, "Audio CD (", 10);
    cdda_format_hex32(ctx->volume_name + 10, cdda_disc_id(ctx));
    ctx->volume_name[18] = ')';
    ctx->volume_name[19] = '\0';
}

/*
 * A CD-Text disc title makes a friendlier volume name than the
 * generic disc-id label. ':' and '/' cannot appear in an AmigaDOS
 * volume name.
 */
static void cdda_set_album_volume_name(cdda_context_t *ctx)
{
    size_t i;

    for (i = 0; i < sizeof(ctx->volume_name) - 1 &&
                ctx->album_title[i] != '\0'; i++) {
        char ch = ctx->album_title[i];

        ctx->volume_name[i] = (ch == ':' || ch == '/') ? '-' : ch;
    }
    ctx->volume_name[i] = '\0';
}

static int cdda_appendf(char *buf, size_t buf_size, size_t *used,
                        const char *fmt, ...)
{
    va_list ap;
    int wrote;

    if (*used >= buf_size)
        return 0;

    va_start(ap, fmt);
    wrote = odfs_vsnprintf(buf + *used, buf_size - *used, fmt, ap);
    va_end(ap);

    if (wrote < 0 || (size_t)wrote >= buf_size - *used)
        return 0;

    *used += (size_t)wrote;
    return 1;
}

static void cdda_generate_cddb(cdda_context_t *ctx)
{
    char *text;
    size_t text_size;
    size_t used = 0;
    uint32_t disc_id;
    uint32_t total_seconds;

    if (ctx->track_count == 0)
        return;

    text_size = 256u + (size_t)ctx->track_count * 48u;
    text = odfs_malloc(text_size);
    if (!text)
        return;

    disc_id = cdda_disc_id(ctx);
    total_seconds = cdda_total_seconds(ctx);

    if (!cdda_appendf(text, text_size, &used,
                      "# Generated by ODFileSystem from disc TOC\n"
                      "DISCID=%08" PRIx32 "\n"
                      "TRACKS=%d\n"
                      "TOTAL_SECONDS=%" PRIu32 "\n"
                      "TOTAL_FRAMES=%" PRIu32 "\n",
                      disc_id, ctx->track_count,
                      total_seconds,
                      total_seconds * CDDA_FRAMES_PER_SEC)) {
        odfs_free(text);
        return;
    }

    for (int i = 0; i < ctx->track_count; i++) {
        if (!cdda_appendf(text, text_size, &used,
                          "OFFSET%02d=%" PRIu32 "\n",
                          i + 1,
                          cdda_track_offset_frames(&ctx->tracks[i]))) {
            odfs_free(text);
            return;
        }
    }

    if (!cdda_appendf(text, text_size, &used,
                      "QUERY=cddb query %08" PRIx32 " %d",
                      disc_id, ctx->track_count)) {
        odfs_free(text);
        return;
    }

    for (int i = 0; i < ctx->track_count; i++) {
        if (!cdda_appendf(text, text_size, &used,
                          " %" PRIu32,
                          cdda_track_offset_frames(&ctx->tracks[i]))) {
            odfs_free(text);
            return;
        }
    }

    if (!cdda_appendf(text, text_size, &used, " %" PRIu32 "\n",
                      total_seconds)) {
        odfs_free(text);
        return;
    }

    ctx->cddb_text = text;
    ctx->cddb_size = used;
}

static int cdda_fill_virtual_node(const void *data, size_t size,
                                  uint32_t id, uint32_t lba,
                                  const char *name, size_t name_size,
                                  odfs_node_t *node)
{
    if (!data || size == 0)
        return 0;

    if (name_size > sizeof(node->name))
        return 0;

    memset(node, 0, sizeof(*node));
    node->id = id;
    node->parent_id = 0;
    node->backend = ODFS_BACKEND_CDDA;
    node->kind = ODFS_NODE_VIRTUAL;
    node->size = size;
    node->extent.lba = lba;
    node->extent.length = (uint32_t)size;
    memcpy(node->name, name, name_size);
    return 1;
}

static int cdda_fill_cddb_node(const cdda_context_t *ctx, odfs_node_t *node)
{
    return cdda_fill_virtual_node(ctx->cddb_text, ctx->cddb_size,
                                  CDDA_CDDB_NODE_ID, CDDA_CDDB_LBA,
                                  CDDA_CDDB_NAME, sizeof(CDDA_CDDB_NAME),
                                  node);
}

static const char *cdda_cdtext_type_name(uint8_t type, uint8_t track)
{
    switch (type) {
    case 0x80: return "TITLE";
    case 0x81: return "PERFORMER";
    case 0x82: return "SONGWRITER";
    case 0x83: return "COMPOSER";
    case 0x84: return "ARRANGER";
    case 0x85: return "MESSAGE";
    case 0x86: return "DISC_ID";
    case 0x87: return "GENRE";
    case 0x88: return "TOC_INFO";
    case 0x89: return "TOC_INFO2";
    case 0x8e: return track == 0 ? "UPC_EAN" : "ISRC";
    case 0x8f: return "SIZE_INFO";
    default:   return "UNKNOWN";
    }
}

static size_t cdda_sanitize_ascii(char *dst, size_t dst_size,
                                  const uint8_t *src, size_t src_size)
{
    size_t used = 0;

    if (dst_size == 0)
        return 0;

    for (size_t i = 0; i < src_size && used + 1 < dst_size; i++) {
        uint8_t ch = src[i];

        if (ch == '\r' || ch == '\n' || ch == '\t')
            ch = ' ';
        if (ch < 0x20 || ch > 0x7e)
            ch = '?';
        dst[used++] = (char)ch;
    }

    dst[used] = '\0';
    return used;
}

static size_t cdda_hex_encode(char *dst, size_t dst_size,
                              const uint8_t *src, size_t src_size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t used = 0;

    if (dst_size == 0)
        return 0;

    for (size_t i = 0; i < src_size && used + 2 < dst_size; i++) {
        dst[used++] = hex[src[i] >> 4];
        dst[used++] = hex[src[i] & 0x0f];
    }

    dst[used] = '\0';
    return used;
}

static int cdda_append_cdtext_record(char *buf, size_t buf_size, size_t *used,
                                     uint8_t type, uint8_t track, uint8_t block,
                                     int is_dbcs, const uint8_t *data,
                                     size_t data_size)
{
    char value[256];
    size_t start = 0;
    int value_count = 0;

    if (track == 0) {
        if (block != 0) {
            if (!cdda_appendf(buf, buf_size, used, "BLOCK%02u.", block))
                return 0;
        }
        if (!cdda_appendf(buf, buf_size, used, "DISC.%s=",
                          cdda_cdtext_type_name(type, track)))
            return 0;
    } else {
        if (block != 0) {
            if (!cdda_appendf(buf, buf_size, used, "BLOCK%02u.", block))
                return 0;
        }
        if (!cdda_appendf(buf, buf_size, used, "TRACK%02u.%s=",
                          track, cdda_cdtext_type_name(type, track)))
            return 0;
    }

    if (is_dbcs) {
        return cdda_appendf(buf, buf_size, used, "<DBCS>\n");
    }

    if (type >= 0x80 && type <= 0x85) {
        while (start < data_size) {
            size_t end = start;
            while (end < data_size && data[end] != 0)
                end++;
            if (end > start) {
                if (value_count > 0) {
                    if (!cdda_appendf(buf, buf_size, used, "; "))
                        return 0;
                }
                cdda_sanitize_ascii(value, sizeof(value),
                                    data + start, end - start);
                if (!cdda_appendf(buf, buf_size, used, "%s", value))
                    return 0;
                value_count++;
            }
            start = end + 1;
        }
        return cdda_appendf(buf, buf_size, used, "\n");
    }

    if (type == 0x8e) {
        cdda_sanitize_ascii(value, sizeof(value), data, data_size);
        return cdda_appendf(buf, buf_size, used, "%s\n", value);
    }

    cdda_hex_encode(value, sizeof(value), data, data_size);
    return cdda_appendf(buf, buf_size, used, "%s\n", value);
}

static void cdda_store_track_title(cdda_context_t *ctx, int track,
                                   const char *title, size_t len)
{
    cdda_track_t *trk = NULL;

    if (track < 0 || len == 0)
        return;

    /* track 0 is the album title; it names the volume, not a file */
    if (track == 0) {
        if (!(len == 1 && title[0] == '\t'))
            cdda_sanitize_ascii(ctx->album_title, sizeof(ctx->album_title),
                                (const uint8_t *)title, len);
        return;
    }

    for (int i = 0; i < ctx->track_count; i++) {
        if (ctx->tracks[i].number == track) {
            trk = &ctx->tracks[i];
            break;
        }
    }
    if (!trk)
        return; /* data track or beyond the TOC */

    /* a title of a single TAB means "same as the previous track" */
    if (len == 1 && title[0] == '\t') {
        for (int i = 0; i < ctx->track_count; i++) {
            if (ctx->tracks[i].number == track - 1) {
                memcpy(trk->title, ctx->tracks[i].title,
                       sizeof(trk->title));
                return;
            }
        }
        return;
    }

    cdda_sanitize_ascii(trk->title, sizeof(trk->title),
                        (const uint8_t *)title, len);
}

/*
 * Decode per-track TITLE strings (pack type 0x80, block 0) into the
 * track table. Characters flow across packs as NUL-separated strings:
 * each NUL ends the current track's title and starts the next track's.
 * pack[1] names the track the pack's first character belongs to and
 * the low nibble of byte 3 is that character's position within the
 * string, so a zero position resynchronizes the walk.
 */
static void cdda_extract_track_titles(cdda_context_t *ctx,
                                      const uint8_t *raw, size_t pack_count)
{
    char cur[ODFS_AMIGA_COMMENT_MAX * 2];
    size_t cur_len = 0;
    int cur_track = -1;
    int skip_current = 0;

    for (size_t i = 0; i < pack_count; i++) {
        const uint8_t *pack = raw + 4u + i * 18u;
        uint8_t type = pack[0];
        uint8_t track = pack[1];
        uint8_t bncp = pack[3];

        if (type != 0x80)
            continue;
        if (((bncp >> 4) & 0x07) != 0)
            continue; /* first character-set block only */
        if (bncp & 0x80)
            continue; /* DBCS is not decoded */

        if ((bncp & 0x0f) == 0) {
            /* pack starts a fresh string: resynchronize */
            cur_track = track;
            cur_len = 0;
            skip_current = 0;
        } else if (cur_track < 0) {
            /* joined mid-string: the head of this title is missing */
            cur_track = track;
            cur_len = 0;
            skip_current = 1;
        }

        for (int b = 0; b < 12; b++) {
            uint8_t ch = pack[4 + b];

            if (ch == 0) {
                if (!skip_current)
                    cdda_store_track_title(ctx, cur_track, cur, cur_len);
                cur_track++;
                cur_len = 0;
                skip_current = 0;
            } else if (cur_len + 1 < sizeof(cur)) {
                cur[cur_len++] = (char)ch;
            }
        }
    }
    /* a string not yet NUL-terminated at the end of the packs is
     * incomplete; drop it rather than store a truncated title */
}

/*
 * Render the string-typed packs (0x80-0x85) of one character-set
 * block as one line per stored string. Characters flow across packs
 * exactly as in cdda_extract_track_titles(); rendering per pack run
 * would attribute the tail of one string to the next track's line.
 * Returns 0 when the text buffer is full.
 */
static int cdda_render_cdtext_strings(const uint8_t *raw, size_t pack_count,
                                      uint8_t type, uint8_t block,
                                      char *buf, size_t buf_size,
                                      size_t *used)
{
    char cur[256];
    char clean[256];
    size_t cur_len = 0;
    int cur_track = -1;
    int skip_current = 0;

    for (size_t i = 0; i < pack_count; i++) {
        const uint8_t *pack = raw + 4u + i * 18u;
        uint8_t bncp = pack[3];

        if (pack[0] != type)
            continue;
        if (((bncp >> 4) & 0x07) != block)
            continue;
        if (bncp & 0x80)
            continue; /* DBCS packs keep the pack-run rendering */

        if ((bncp & 0x0f) == 0) {
            /* pack starts a fresh string: resynchronize */
            cur_track = pack[1];
            cur_len = 0;
            skip_current = 0;
        } else if (cur_track < 0) {
            /* joined mid-string: the head of this string is missing */
            cur_track = pack[1];
            cur_len = 0;
            skip_current = 1;
        }

        for (int b = 0; b < 12; b++) {
            uint8_t ch = pack[4 + b];

            if (ch != 0) {
                if (cur_len + 1 < sizeof(cur))
                    cur[cur_len++] = (char)ch;
                continue;
            }

            if (!skip_current && cur_len != 0) {
                cdda_sanitize_ascii(clean, sizeof(clean),
                                    (const uint8_t *)cur, cur_len);
                if (block != 0 &&
                    !cdda_appendf(buf, buf_size, used, "BLOCK%02u.", block))
                    return 0;
                if (cur_track == 0) {
                    if (!cdda_appendf(buf, buf_size, used, "DISC.%s=%s\n",
                                      cdda_cdtext_type_name(type, 0), clean))
                        return 0;
                } else if (!cdda_appendf(buf, buf_size, used,
                                         "TRACK%02u.%s=%s\n", cur_track,
                                         cdda_cdtext_type_name(type,
                                                        (uint8_t)cur_track),
                                         clean)) {
                    return 0;
                }
            }
            cur_track++;
            cur_len = 0;
            skip_current = 0;
        }
    }
    return 1;
}

static void cdda_generate_cdtext(cdda_context_t *ctx)
{
    uint8_t *raw = NULL;
    size_t raw_size = 0;
    char *text = NULL;
    uint8_t current_type = 0;
    uint8_t current_track = 0;
    uint8_t current_block = 0;
    int current_dbcs = 0;
    int have_current = 0;
    size_t field_used = 0;
    size_t used = 0;
    size_t pack_count;
    uint8_t *field_buf = NULL;
    size_t field_cap;
    odfs_err_t err;

    if (!ctx->media)
        return;

    err = odfs_media_read_cdtext(ctx->media, &raw, &raw_size);
    if (err != ODFS_OK || !raw || raw_size <= 4)
        return;

    pack_count = (raw_size - 4u) / 18u;
    if (pack_count == 0) {
        odfs_free(raw);
        return;
    }

    cdda_extract_track_titles(ctx, raw, pack_count);

    field_cap = pack_count * 12u;
    field_buf = odfs_malloc(field_cap);
    text = odfs_malloc(128u + raw_size * 4u);
    if (!field_buf || !text) {
        odfs_free(field_buf);
        odfs_free(text);
        odfs_free(raw);
        return;
    }

    if (!cdda_appendf(text, 128u + raw_size * 4u, &used,
                      "# Generated by ODFileSystem from CD-Text packs\n")) {
        odfs_free(field_buf);
        odfs_free(text);
        odfs_free(raw);
        return;
    }

    for (uint8_t type = 0x80; type <= 0x85; type++) {
        for (uint8_t block = 0; block < 8; block++) {
            if (!cdda_render_cdtext_strings(raw, pack_count, type, block,
                                            text, 128u + raw_size * 4u,
                                            &used)) {
                odfs_free(field_buf);
                odfs_free(text);
                odfs_free(raw);
                return;
            }
        }
    }

    for (size_t i = 0; i < pack_count; i++) {
        const uint8_t *pack = raw + 4u + i * 18u;
        uint8_t type = pack[0];
        uint8_t track = pack[1];
        uint8_t bncp = pack[3];
        uint8_t block = (bncp >> 4) & 0x07;
        int is_dbcs = (bncp & 0x80) != 0;

        /* string types were rendered above, one line per string */
        if (type >= 0x80 && type <= 0x85 && !is_dbcs)
            continue;

        if (!have_current ||
            type != current_type ||
            track != current_track ||
            block != current_block ||
            is_dbcs != current_dbcs) {
            if (have_current &&
                !cdda_append_cdtext_record(text, 128u + raw_size * 4u, &used,
                                           current_type, current_track,
                                           current_block, current_dbcs,
                                           field_buf, field_used)) {
                odfs_free(field_buf);
                odfs_free(text);
                odfs_free(raw);
                return;
            }
            have_current = 1;
            current_type = type;
            current_track = track;
            current_block = block;
            current_dbcs = is_dbcs;
            field_used = 0;
        }

        if (field_used + 12u <= field_cap) {
            memcpy(field_buf + field_used, pack + 4, 12u);
            field_used += 12u;
        }
    }

    if (have_current &&
        !cdda_append_cdtext_record(text, 128u + raw_size * 4u, &used,
                                   current_type, current_track,
                                   current_block, current_dbcs,
                                   field_buf, field_used)) {
        odfs_free(field_buf);
        odfs_free(text);
        odfs_free(raw);
        return;
    }

    ctx->cdtext_text = text;
    ctx->cdtext_size = used;

    odfs_free(field_buf);
    odfs_free(raw);
}

static int cdda_fill_cdtext_node(const cdda_context_t *ctx, odfs_node_t *node)
{
    return cdda_fill_virtual_node(ctx->cdtext_text, ctx->cdtext_size,
                                  CDDA_CDTEXT_NODE_ID, CDDA_CDTEXT_LBA,
                                  CDDA_CDTEXT_NAME,
                                  sizeof(CDDA_CDTEXT_NAME), node);
}

static int cdda_fill_disk_icon_node(const cdda_context_t *ctx,
                                    odfs_node_t *node)
{
    if (ctx->is_mixed_mode || !ctx->disk_icon || ctx->disk_icon_size == 0)
        return 0;

    return cdda_fill_virtual_node(ctx->disk_icon, ctx->disk_icon_size,
                                  CDDA_DISK_ICON_NODE_ID,
                                  CDDA_DISK_ICON_LBA,
                                  CDDA_DISK_ICON_NAME,
                                  sizeof(CDDA_DISK_ICON_NAME), node);
}

static int cdda_metadata_count(const cdda_context_t *ctx)
{
    int count = 0;

    if (!ctx->is_mixed_mode && ctx->disk_icon &&
        ctx->disk_icon_size != 0)
        count++;
    if (ctx->cddb_text && ctx->cddb_size != 0)
        count++;
    if (ctx->cdtext_text && ctx->cdtext_size != 0)
        count++;
    return count;
}

static int cdda_fill_metadata_node(const cdda_context_t *ctx, int index,
                                   odfs_node_t *node)
{
    if (!ctx->is_mixed_mode && ctx->disk_icon &&
        ctx->disk_icon_size != 0) {
        if (index == 0)
            return cdda_fill_disk_icon_node(ctx, node);
        index--;
    }
    if (ctx->cddb_text && ctx->cddb_size != 0) {
        if (index == 0)
            return cdda_fill_cddb_node(ctx, node);
        index--;
    }
    if (ctx->cdtext_text && ctx->cdtext_size != 0) {
        if (index == 0)
            return cdda_fill_cdtext_node(ctx, node);
    }
    return 0;
}

static void cdda_byteswap_samples(uint8_t *buf, size_t len)
{
    size_t i;

    for (i = 0; i + 1 < len; i += 2) {
        uint8_t tmp = buf[i];
        buf[i] = buf[i + 1];
        buf[i + 1] = tmp;
    }
}

static int cdda_audio_cache_contains(const cdda_context_t *ctx, uint32_t lba)
{
    return ctx->audio_cache != NULL &&
           ctx->audio_cache_frames != 0 &&
           lba >= ctx->audio_cache_lba &&
           lba < ctx->audio_cache_lba + ctx->audio_cache_frames;
}

static odfs_err_t cdda_fill_audio_cache(cdda_context_t *ctx,
                                        const cdda_track_t *trk,
                                        uint32_t frame_num)
{
    uint32_t frames;
    uint32_t start_lba;
    odfs_err_t err;

    if (!ctx->audio_cache || !ctx->media || !ctx->media->ops->read_audio)
        return ODFS_ERR_UNSUPPORTED;
    if (frame_num >= trk->length_frames)
        return ODFS_ERR_RANGE;

    frames = trk->length_frames - frame_num;
    if (frames > CDDA_READAHEAD_FRAMES)
        frames = CDDA_READAHEAD_FRAMES;

    start_lba = trk->start_lba + frame_num;
    err = odfs_media_read_audio(ctx->media, start_lba, frames,
                                ctx->audio_cache);
    if (err != ODFS_OK) {
        ctx->audio_cache_frames = 0;
        return err;
    }

    if (ctx->file_format == CDDA_FILE_FORMAT_AIFF)
        cdda_byteswap_samples(ctx->audio_cache,
                              (size_t)frames * CDDA_FRAME_SIZE);

    ctx->audio_cache_lba = start_lba;
    ctx->audio_cache_frames = frames;
    return ODFS_OK;
}

/*
 * Build a 44-byte PCM WAV header for the given data size.
 */
static void cdda_build_wav_header(uint8_t *hdr, uint32_t data_size)
{
    uint32_t byte_rate = CDDA_SAMPLE_RATE * CDDA_CHANNELS * (CDDA_BITS_PER_SAMPLE / 8);
    uint16_t block_align = CDDA_CHANNELS * (CDDA_BITS_PER_SAMPLE / 8);

    memcpy(&hdr[0], "RIFF", 4);
    cdda_write_le32(&hdr[4], 36 + data_size);  /* file size - 8 */
    memcpy(&hdr[8], "WAVE", 4);
    memcpy(&hdr[12], "fmt ", 4);
    cdda_write_le32(&hdr[16], 16);              /* fmt chunk size */
    cdda_write_le16(&hdr[20], 1);               /* PCM format */
    cdda_write_le16(&hdr[22], CDDA_CHANNELS);
    cdda_write_le32(&hdr[24], CDDA_SAMPLE_RATE);
    cdda_write_le32(&hdr[28], byte_rate);
    cdda_write_le16(&hdr[32], block_align);
    cdda_write_le16(&hdr[34], CDDA_BITS_PER_SAMPLE);
    memcpy(&hdr[36], "data", 4);
    cdda_write_le32(&hdr[40], data_size);
}

static void cdda_build_aiff_header(uint8_t *hdr, uint32_t data_size)
{
    static const uint8_t rate_44100[10] = {
        0x40, 0x0e, 0xac, 0x44, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    uint32_t sample_frames = data_size /
                             (CDDA_CHANNELS * (CDDA_BITS_PER_SAMPLE / 8));

    memcpy(&hdr[0], "FORM", 4);
    cdda_write_be32(&hdr[4], 46 + data_size);
    memcpy(&hdr[8], "AIFF", 4);
    memcpy(&hdr[12], "COMM", 4);
    cdda_write_be32(&hdr[16], 18);
    cdda_write_be16(&hdr[20], CDDA_CHANNELS);
    cdda_write_be32(&hdr[22], sample_frames);
    cdda_write_be16(&hdr[26], CDDA_BITS_PER_SAMPLE);
    memcpy(&hdr[28], rate_44100, sizeof(rate_44100));
    memcpy(&hdr[38], "SSND", 4);
    cdda_write_be32(&hdr[42], 8 + data_size);
    cdda_write_be32(&hdr[46], 0);
    cdda_write_be32(&hdr[50], 0);
}

static void cdda_build_header(cdda_file_format_t format,
                              uint8_t *hdr,
                              uint32_t data_size)
{
    if (format == CDDA_FILE_FORMAT_AIFF)
        cdda_build_aiff_header(hdr, data_size);
    else
        cdda_build_wav_header(hdr, data_size);
}

/* ------------------------------------------------------------------ */
/* node IDs and encoding                                               */
/* ------------------------------------------------------------------ */

/*
 * Node ID encoding for CDDA:
 *   0          = root (or CDDA/ virtual dir on mixed-mode)
 *   1..99      = track number
 *   extent.lba = track index + 2 into tracks[] array (for quick lookup)
 */

static void cdda_track_name(int track_num,
                            cdda_file_format_t format,
                            char *buf,
                            size_t buf_size)
{
    const char *ext = (format == CDDA_FILE_FORMAT_AIFF) ? "aiff" : "wav";
    int len = 0;

    (void)buf_size;

    buf[len++] = 'T';
    buf[len++] = 'r';
    buf[len++] = 'a';
    buf[len++] = 'c';
    buf[len++] = 'k';
    buf[len++] = '0' + (track_num / 10);
    buf[len++] = '0' + (track_num % 10);
    buf[len++] = '.';
    while (*ext)
        buf[len++] = *ext++;
    buf[len] = '\0';
}

/* ------------------------------------------------------------------ */
/* probe                                                               */
/* ------------------------------------------------------------------ */

static odfs_err_t cdda_probe(odfs_cache_t *cache,
                               odfs_log_state_t *log,
                               uint32_t session_start)
{
    (void)cache;
    (void)session_start;

    /*
     * CDDA detection requires a TOC from the device. We can't probe
     * from the cache alone — the media layer's read_toc must work.
     * Since probe() doesn't have access to the media handle, we
     * always return BAD_FORMAT here. CDDA activation is handled
     * specially in mount.c after all other backends have been tried.
     *
     * TODO: add a cdda_probe_media() that takes odfs_media_t*
     * and is called from mount.c directly.
     */
    ODFS_DEBUG(log, ODFS_SUB_CDDA,
                "CDDA probe: requires TOC (not available from cache)");
    return ODFS_ERR_BAD_FORMAT;
}

/* ------------------------------------------------------------------ */
/* mount                                                               */
/* ------------------------------------------------------------------ */

/*
 * Mount a CDDA "filesystem" from a TOC. This is called directly
 * from mount.c, not through the standard probe→mount path, because
 * CDDA needs the media handle for TOC reading.
 *
 * Returns ODFS_OK if audio tracks were found.
 */
odfs_err_t cdda_mount_from_toc(const odfs_toc_t *toc,
                               int has_data_session,
                               const odfs_mount_opts_t *opts,
                               odfs_media_t *media,
                               odfs_node_t *root_out,
                               void **backend_ctx)
{
    cdda_context_t *ctx;
    int audio_count = 0;
    uint32_t header_size;

    ctx = odfs_calloc(1, sizeof(*ctx));
    if (!ctx)
        return ODFS_ERR_NOMEM;

    ctx->is_mixed_mode = has_data_session;
    ctx->file_format = (opts && opts->prefer_aiff)
        ? CDDA_FILE_FORMAT_AIFF
        : CDDA_FILE_FORMAT_WAV;
    ctx->media = media;
    header_size = cdda_header_size(ctx->file_format);

    for (int i = 0; i < toc->session_count && audio_count < CDDA_MAX_TRACKS; i++) {
        uint32_t start = toc->sessions[i].start_lba;
        uint32_t length = toc->sessions[i].length;

        if ((toc->sessions[i].control & 0x04) != 0)
            continue;

        if (length == 0 && i + 1 < toc->session_count)
            length = toc->sessions[i + 1].start_lba - start;
        if (length == 0 && toc->leadout_lba > start)
            length = toc->leadout_lba - start;

        if (length == 0)
            continue;

        ctx->tracks[audio_count].number = toc->sessions[i].number;
        ctx->tracks[audio_count].start_lba = start;
        ctx->tracks[audio_count].length_frames = length;
        ctx->tracks[audio_count].data_size = (uint64_t)length * CDDA_FRAME_SIZE;
        ctx->tracks[audio_count].file_size = ctx->tracks[audio_count].data_size
                                             + header_size;
        audio_count++;
    }

    ctx->track_count = audio_count;

    if (audio_count == 0) {
        odfs_free(ctx);
        return ODFS_ERR_BAD_FORMAT;
    }

    cdda_set_volume_name(ctx);

    ctx->audio_cache = odfs_malloc((size_t)CDDA_READAHEAD_FRAMES *
                                   CDDA_FRAME_SIZE);

    cdda_generate_cddb(ctx);
    cdda_generate_cdtext(ctx);

    if (!ctx->is_mixed_mode && ctx->album_title[0] != '\0')
        cdda_set_album_volume_name(ctx);

    /* build root node */
    memset(root_out, 0, sizeof(*root_out));
    root_out->id = 0;
    root_out->parent_id = 0;
    root_out->backend = ODFS_BACKEND_CDDA;
    root_out->kind = ODFS_NODE_DIR;
    if (ctx->is_mixed_mode) {
        memcpy(root_out->name, "CDDA", 5);
    } else {
        root_out->name[0] = '/';
        root_out->name[1] = '\0';
    }
    root_out->extent.lba = 0;
    root_out->extent.length = 0;

    *backend_ctx = ctx;
    return ODFS_OK;
}

static odfs_err_t cdda_mount(odfs_cache_t *cache,
                               odfs_log_state_t *log,
                               uint32_t session_start,
                               odfs_node_t *root_out,
                               void **backend_ctx)
{
    (void)cache;
    (void)log;
    (void)session_start;
    (void)root_out;
    (void)backend_ctx;
    /* standard mount path not used — see cdda_mount_from_toc() */
    return ODFS_ERR_UNSUPPORTED;
}

static void cdda_unmount(void *backend_ctx)
{
    cdda_context_t *ctx = backend_ctx;

    if (ctx) {
        odfs_free(ctx->audio_cache);
        odfs_free(ctx->cddb_text);
        odfs_free(ctx->cdtext_text);
        odfs_free(ctx->disk_icon);
        odfs_free(ctx);
    }
}

/* ------------------------------------------------------------------ */
/* readdir                                                             */
/* ------------------------------------------------------------------ */

static void cdda_fill_track_node(const cdda_context_t *ctx, int i,
                                 odfs_node_t *node)
{
    memset(node, 0, sizeof(*node));
    node->id = ctx->tracks[i].number;
    node->parent_id = 0;
    node->backend = ODFS_BACKEND_CDDA;
    node->kind = ODFS_NODE_VIRTUAL;
    node->size = ctx->tracks[i].file_size;
    node->extent.lba = (uint32_t)(i + 2);
    node->extent.length = (uint32_t)ctx->tracks[i].file_size;

    cdda_track_name(ctx->tracks[i].number, ctx->file_format,
                    node->name, sizeof(node->name));

    /* CD-Text track title becomes the AmigaDOS file comment */
    if (ctx->tracks[i].title[0] != '\0') {
        memcpy(node->amiga_as.comment, ctx->tracks[i].title,
               sizeof(node->amiga_as.comment));
        node->amiga_as.has_comment = 1;
    }
}

static odfs_err_t cdda_readdir(void *backend_ctx,
                                 odfs_cache_t *cache,
                                 odfs_log_state_t *log,
                                 const odfs_node_t *dir,
                                 odfs_dir_iter_fn callback,
                                 void *cb_ctx,
                                 uint32_t *resume_offset)
{
    cdda_context_t *ctx = backend_ctx;
    (void)cache;
    (void)log;
    (void)dir;

    int start = (resume_offset && *resume_offset) ? (int)*resume_offset : 0;
    int metadata_count = cdda_metadata_count(ctx);

    for (int i = start; i < metadata_count; i++) {
        odfs_node_t node;

        if (!cdda_fill_metadata_node(ctx, i, &node))
            continue;

        odfs_err_t err = callback(&node, cb_ctx);
        if (err != ODFS_OK) {
            if (resume_offset)
                *resume_offset = (uint32_t)i;
            return err;
        }
    }

    for (int i = start - metadata_count; i < ctx->track_count; i++) {
        odfs_node_t node;
        if (i < 0)
            continue;

        cdda_fill_track_node(ctx, i, &node);

        odfs_err_t err = callback(&node, cb_ctx);
        if (err != ODFS_OK) {
            if (resume_offset)
                *resume_offset = (uint32_t)(metadata_count + i);
            return err;
        }
    }

    if (resume_offset)
        *resume_offset = (uint32_t)(metadata_count + ctx->track_count);
    return ODFS_OK;
}

/* ------------------------------------------------------------------ */
/* read — synthesize header + read audio frames                        */
/* ------------------------------------------------------------------ */

static odfs_err_t cdda_read(void *backend_ctx,
                              odfs_cache_t *cache,
                              odfs_log_state_t *log,
                              const odfs_node_t *file,
                              uint64_t offset,
                              void *buf,
                              size_t *len)
{
    cdda_context_t *ctx = backend_ctx;
    (void)cache;
    (void)log;

    if (file->id == CDDA_CDDB_NODE_ID) {
        size_t want = *len;

        if (!ctx->cddb_text || offset >= ctx->cddb_size) {
            *len = 0;
            return ODFS_OK;
        }
        if (offset + want > ctx->cddb_size)
            want = ctx->cddb_size - (size_t)offset;

        memcpy(buf, ctx->cddb_text + (size_t)offset, want);
        *len = want;
        return ODFS_OK;
    }

    if (file->id == CDDA_CDTEXT_NODE_ID) {
        size_t want = *len;

        if (!ctx->cdtext_text || offset >= ctx->cdtext_size) {
            *len = 0;
            return ODFS_OK;
        }
        if (offset + want > ctx->cdtext_size)
            want = ctx->cdtext_size - (size_t)offset;

        memcpy(buf, ctx->cdtext_text + (size_t)offset, want);
        *len = want;
        return ODFS_OK;
    }

    if (file->id == CDDA_DISK_ICON_NODE_ID) {
        size_t want = *len;

        if (!ctx->disk_icon || offset >= ctx->disk_icon_size) {
            *len = 0;
            return ODFS_OK;
        }
        if (offset + want > ctx->disk_icon_size)
            want = ctx->disk_icon_size - (size_t)offset;

        memcpy(buf, ctx->disk_icon + (size_t)offset, want);
        *len = want;
        return ODFS_OK;
    }

    int track_idx = (int)file->extent.lba - 2;
    if (track_idx < 0 || track_idx >= ctx->track_count) {
        *len = 0;
        return ODFS_ERR_NOT_FOUND;
    }

    cdda_track_t *trk = &ctx->tracks[track_idx];
    uint32_t header_size = cdda_header_size(ctx->file_format);
    size_t want = *len;
    size_t done = 0;
    uint8_t *out = buf;

    if (offset >= trk->file_size) {
        *len = 0;
        return ODFS_OK;
    }
    if (offset + want > trk->file_size)
        want = (size_t)(trk->file_size - offset);

    /* serve synthesized header bytes if offset is within the header */
    if (offset < header_size) {
        uint8_t hdr[CDDA_AIFF_HEADER_SIZE];
        cdda_build_header(ctx->file_format, hdr, (uint32_t)trk->data_size);

        size_t hdr_avail = header_size - (size_t)offset;
        size_t chunk = (hdr_avail < want) ? hdr_avail : want;
        memcpy(out, hdr + offset, chunk);
        done += chunk;
    }

    /*
     * Audio data: read via SCSI Read CD (0xBE). Any failure is returned
     * to the caller; do not mask it by synthesizing silence.
     */
    while (done < want) {
        uint64_t audio_pos64 = (offset + done) - header_size;
        uint32_t audio_pos;
        uint32_t frame_num;
        uint32_t frame_off;

        /* an audio track is far below 4 GiB, so 32-bit math suffices;
         * this keeps the 64-bit division helpers out of the binary */
        if (audio_pos64 > 0xffffffffUL) {
            *len = 0;
            return ODFS_ERR_RANGE;
        }
        audio_pos = (uint32_t)audio_pos64;
        frame_num = audio_pos / CDDA_FRAME_SIZE;
        frame_off = audio_pos % CDDA_FRAME_SIZE;
        uint32_t start_frame = trk->start_lba + frame_num;

        if (!ctx->media || !ctx->media->ops->read_audio) {
            ODFS_ERROR(log, ODFS_SUB_CDDA,
                       "audio read unavailable track=%d lba=%" PRIu32,
                       trk->number, start_frame);
            *len = 0;
            return ODFS_ERR_UNSUPPORTED;
        }

        if (ctx->audio_cache) {
            size_t cache_off;
            size_t avail;
            size_t chunk;
            odfs_err_t err;

            if (!cdda_audio_cache_contains(ctx, start_frame)) {
                err = cdda_fill_audio_cache(ctx, trk, frame_num);
                if (err != ODFS_OK) {
                    ODFS_ERROR(log, ODFS_SUB_CDDA,
                               "audio read failed track=%d lba=%" PRIu32
                               " err=%s",
                               trk->number, start_frame, odfs_err_str(err));
                    *len = 0;
                    return err;
                }
            }

            cache_off = (size_t)(start_frame - ctx->audio_cache_lba) *
                        CDDA_FRAME_SIZE + frame_off;
            avail = (size_t)ctx->audio_cache_frames * CDDA_FRAME_SIZE -
                    cache_off;
            chunk = (avail < want - done) ? avail : want - done;
            memcpy(out + done, ctx->audio_cache + cache_off, chunk);
            done += chunk;
            continue;
        }

        {
            uint8_t frame_buf[CDDA_FRAME_SIZE];
            odfs_err_t err = odfs_media_read_audio(ctx->media,
                                                   start_frame, 1,
                                                   frame_buf);
            if (err != ODFS_OK) {
                ODFS_ERROR(log, ODFS_SUB_CDDA,
                           "audio read failed track=%d lba=%" PRIu32
                           " err=%s",
                           trk->number, start_frame, odfs_err_str(err));
                *len = 0;
                return err;
            }

            if (ctx->file_format == CDDA_FILE_FORMAT_AIFF)
                cdda_byteswap_samples(frame_buf, sizeof(frame_buf));

            size_t avail = CDDA_FRAME_SIZE - frame_off;
            size_t chunk = (avail < want - done) ? avail : want - done;
            memcpy(out + done, frame_buf + frame_off, chunk);
            done += chunk;
        }
    }

    *len = done;
    return ODFS_OK;
}

/* ------------------------------------------------------------------ */
/* lookup                                                              */
/* ------------------------------------------------------------------ */

static odfs_err_t cdda_lookup(void *backend_ctx,
                                odfs_cache_t *cache,
                                odfs_log_state_t *log,
                                const odfs_node_t *dir,
                                const char *name,
                                odfs_node_t *out)
{
    cdda_context_t *ctx = backend_ctx;
    (void)cache;
    (void)log;
    (void)dir;

    if (odfs_strcasecmp(name, CDDA_CDDB_NAME) == 0) {
        if (cdda_fill_cddb_node(ctx, out))
            return ODFS_OK;
        return ODFS_ERR_NOT_FOUND;
    }

    if (odfs_strcasecmp(name, CDDA_CDTEXT_NAME) == 0) {
        if (cdda_fill_cdtext_node(ctx, out))
            return ODFS_OK;
        return ODFS_ERR_NOT_FOUND;
    }

    if (odfs_strcasecmp(name, CDDA_DISK_ICON_NAME) == 0) {
        if (cdda_fill_disk_icon_node(ctx, out))
            return ODFS_OK;
        return ODFS_ERR_NOT_FOUND;
    }

    for (int i = 0; i < ctx->track_count; i++) {
        char tname[32];
        cdda_track_name(ctx->tracks[i].number, ctx->file_format,
                        tname, sizeof(tname));
        if (odfs_strcasecmp(name, tname) == 0) {
            cdda_fill_track_node(ctx, i, out);
            return ODFS_OK;
        }
    }

    return ODFS_ERR_NOT_FOUND;
}

/* ------------------------------------------------------------------ */
/* get_volume_name                                                     */
/* ------------------------------------------------------------------ */

static odfs_err_t cdda_get_volume_name(void *backend_ctx,
                                         char *buf, size_t buf_size)
{
    cdda_context_t *ctx = backend_ctx;
    size_t len;

    if (buf_size == 0)
        return ODFS_ERR_RANGE;

    len = strlen(ctx->volume_name);
    if (len >= buf_size)
        len = buf_size - 1;
    memcpy(buf, ctx->volume_name, len);
    buf[len] = '\0';

    return ODFS_OK;
}

/* ------------------------------------------------------------------ */
/* get_volume_size                                                     */
/* ------------------------------------------------------------------ */

static uint32_t cdda_get_volume_size(void *backend_ctx)
{
    cdda_context_t *ctx = backend_ctx;
    uint64_t bytes = 0;

    bytes += ctx->cddb_size;
    bytes += ctx->cdtext_size;
    for (int i = 0; i < ctx->track_count; i++)
        bytes += ctx->tracks[i].file_size;

    uint64_t blocks = (bytes + 2047u) / 2048u;
    return blocks > UINT32_MAX ? UINT32_MAX : (uint32_t)blocks;
}

/* ------------------------------------------------------------------ */
/* backend ops table                                                   */
/* ------------------------------------------------------------------ */

const odfs_backend_ops_t cdda_backend_ops = {
    .name            = "cdda",
    .backend_type    = ODFS_BACKEND_CDDA,
    .probe           = cdda_probe,
    .mount           = cdda_mount,
    .unmount         = cdda_unmount,
    .readdir         = cdda_readdir,
    .read            = cdda_read,
    .lookup          = cdda_lookup,
    .get_volume_name = cdda_get_volume_name,
    .get_volume_size = cdda_get_volume_size,
};
