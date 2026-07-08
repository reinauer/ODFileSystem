/*
 * file_media.c — host-side file-backed media implementation
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "odfs/media.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HOST_SECTOR_SIZE 2048
#define HOST_MAX_CUE_TRACKS 99
#define HOST_CUE_TEXT_MAX 128
#define HOST_AUDIO_FRAME_SIZE 2352

typedef struct cue_track {
    uint32_t start_lba;
    uint32_t sector_count;
    uint32_t raw_sector_size;
    uint32_t data_offset;
    uint64_t file_offset;
    uint8_t  number;
    uint8_t  is_audio;
    char     title[HOST_CUE_TEXT_MAX];
    char     performer[HOST_CUE_TEXT_MAX];
} cue_track_t;

typedef struct file_media_ctx {
    FILE       *fp;
    uint32_t    sector_count;
    int         is_cue;
    int         has_audio;
    char        disc_title[HOST_CUE_TEXT_MAX];
    char        disc_performer[HOST_CUE_TEXT_MAX];
    size_t      track_count;
    cue_track_t tracks[HOST_MAX_CUE_TRACKS];
} file_media_ctx_t;

typedef struct cue_track_spec {
    uint32_t start_lba;
    uint32_t raw_sector_size;
    uint32_t data_offset;
    int      have_index;
    uint8_t  number;
    uint8_t  is_audio;
    char     title[HOST_CUE_TEXT_MAX];
    char     performer[HOST_CUE_TEXT_MAX];
} cue_track_spec_t;

static const odfs_media_ops_t file_media_ops;

static int host_ascii_tolower(int ch)
{
    if (ch >= 'A' && ch <= 'Z')
        return ch + ('a' - 'A');
    return ch;
}

static int host_ext_eq(const char *path, const char *ext)
{
    size_t path_len;
    size_t ext_len;
    size_t i;

    if (!path || !ext)
        return 0;

    path_len = strlen(path);
    ext_len = strlen(ext);
    if (path_len < ext_len)
        return 0;

    for (i = 0; i < ext_len; i++) {
        if (host_ascii_tolower((unsigned char)path[path_len - ext_len + i]) !=
            host_ascii_tolower((unsigned char)ext[i]))
            return 0;
    }
    return 1;
}

static char *host_strdup(const char *s)
{
    size_t len;
    char *copy;

    if (!s)
        return NULL;

    len = strlen(s) + 1u;
    copy = malloc(len);
    if (!copy)
        return NULL;
    memcpy(copy, s, len);
    return copy;
}

static char *host_join_relative_path(const char *base_path, const char *name)
{
    const char *slash;
    size_t dir_len;
    size_t name_len;
    char *joined;

    if (!base_path || !name)
        return NULL;

    if (name[0] == '/')
        return host_strdup(name);

    slash = strrchr(base_path, '/');
    dir_len = slash ? (size_t)(slash - base_path + 1) : 0u;
    name_len = strlen(name);

    joined = malloc(dir_len + name_len + 1u);
    if (!joined)
        return NULL;

    if (dir_len != 0u)
        memcpy(joined, base_path, dir_len);
    memcpy(joined + dir_len, name, name_len + 1u);
    return joined;
}

static char *host_trim(char *s)
{
    char *end;

    while (*s != '\0' && isspace((unsigned char)*s))
        s++;

    end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        end--;
    *end = '\0';
    return s;
}

static int cue_parse_msf(const char *s, uint32_t *frames_out)
{
    unsigned int mm, ss, ff;

    if (sscanf(s, "%2u:%2u:%2u", &mm, &ss, &ff) != 3)
        return 0;
    *frames_out = (mm * 60u + ss) * 75u + ff;
    return 1;
}

static int cue_parse_track_layout(const char *mode,
                                  uint32_t *raw_sector_size,
                                  uint32_t *data_offset,
                                  uint8_t *is_audio)
{
    *is_audio = 0;
    if (strcmp(mode, "MODE1/2048") == 0) {
        *raw_sector_size = 2048u;
        *data_offset = 0u;
        return 1;
    }
    if (strcmp(mode, "MODE1/2352") == 0) {
        *raw_sector_size = 2352u;
        *data_offset = 16u;
        return 1;
    }
    if (strcmp(mode, "MODE2/2352") == 0) {
        *raw_sector_size = 2352u;
        *data_offset = 24u;
        return 1;
    }
    if (strcmp(mode, "AUDIO") == 0) {
        *raw_sector_size = HOST_AUDIO_FRAME_SIZE;
        *data_offset = 0u;
        *is_audio = 1;
        return 1;
    }
    return 0;
}

static int cue_parse_file_line(const char *line, char *file_name, size_t file_name_size)
{
    const char *start;
    const char *end;
    size_t len;

    start = strchr(line, '"');
    if (!start)
        return 0;
    start++;

    end = strchr(start, '"');
    if (!end)
        return 0;

    len = (size_t)(end - start);
    if (len + 1u > file_name_size)
        return 0;

    memcpy(file_name, start, len);
    file_name[len] = '\0';
    return 1;
}

/*
 * Copy the value of a TITLE/PERFORMER line: the quoted string when
 * quotes are present, the bare remainder of the line otherwise.
 */
static void cue_parse_text_value(const char *s, char *dst, size_t dst_size)
{
    const char *start;
    const char *end;
    size_t len;

    while (*s != '\0' && isspace((unsigned char)*s))
        s++;

    if (*s == '"') {
        start = s + 1;
        end = strchr(start, '"');
        if (!end)
            end = start + strlen(start);
    } else {
        start = s;
        end = start + strlen(start);
    }

    len = (size_t)(end - start);
    if (len >= dst_size)
        len = dst_size - 1u;
    memcpy(dst, start, len);
    dst[len] = '\0';
}

/*
 * When the payload behind a cue sheet is a RIFF/WAVE file (EAC-style
 * rips), the CD frames live in its "data" chunk. Locate that chunk so
 * track offsets can skip the header. Returns 0 when the file is not a
 * WAV; the caller then treats it as a raw bin.
 */
static int host_riff_data_range(FILE *fp, uint64_t *base_out, uint64_t *size_out)
{
    uint8_t hdr[12];
    uint8_t chdr[8];

    rewind(fp);
    if (fread(hdr, 1, sizeof(hdr), fp) != sizeof(hdr))
        return 0;
    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0)
        return 0;

    for (;;) {
        uint32_t csize;
        long pos;

        if (fread(chdr, 1, sizeof(chdr), fp) != sizeof(chdr))
            return 0;
        csize = (uint32_t)chdr[4] | ((uint32_t)chdr[5] << 8) |
                ((uint32_t)chdr[6] << 16) | ((uint32_t)chdr[7] << 24);
        if (memcmp(chdr, "data", 4) == 0) {
            pos = ftell(fp);
            if (pos < 0)
                return 0;
            *base_out = (uint64_t)pos;
            *size_out = csize;
            return 1;
        }
        /* chunks are word-aligned */
        if (fseek(fp, (long)(csize + (csize & 1u)), SEEK_CUR) != 0)
            return 0;
    }
}

static odfs_err_t cue_media_open(const char *path, odfs_media_t *out)
{
    char line[512];
    char current_file[512];
    char first_file[512];
    char disc_title[HOST_CUE_TEXT_MAX];
    char disc_performer[HOST_CUE_TEXT_MAX];
    cue_track_spec_t specs[HOST_MAX_CUE_TRACKS];
    file_media_ctx_t *fm;
    FILE *cue_fp;
    char *bin_path = NULL;
    size_t track_count = 0;
    size_t i;
    long file_size;
    uint64_t payload_base = 0;
    uint64_t payload_size;
    uint32_t sector_count = 0;

    memset(specs, 0, sizeof(specs));
    current_file[0] = '\0';
    first_file[0] = '\0';
    disc_title[0] = '\0';
    disc_performer[0] = '\0';

    cue_fp = fopen(path, "r");
    if (!cue_fp)
        return ODFS_ERR_IO;

    while (fgets(line, sizeof(line), cue_fp) != NULL) {
        char *trimmed = host_trim(line);

        if (*trimmed == '\0')
            continue;

        if (strncmp(trimmed, "FILE", 4) == 0 && isspace((unsigned char)trimmed[4])) {
            if (!cue_parse_file_line(trimmed, current_file, sizeof(current_file))) {
                fclose(cue_fp);
                return ODFS_ERR_BAD_FORMAT;
            }
            if (first_file[0] == '\0') {
                memcpy(first_file, current_file, sizeof(first_file));
            } else if (strcmp(first_file, current_file) != 0) {
                fclose(cue_fp);
                return ODFS_ERR_UNSUPPORTED;
            }
            continue;
        }

        if (strncmp(trimmed, "TRACK", 5) == 0 && isspace((unsigned char)trimmed[5])) {
            unsigned int track_num;
            char mode[32];

            if (track_count >= HOST_MAX_CUE_TRACKS) {
                fclose(cue_fp);
                return ODFS_ERR_RANGE;
            }
            if (current_file[0] == '\0') {
                fclose(cue_fp);
                return ODFS_ERR_BAD_FORMAT;
            }
            if (sscanf(trimmed, "TRACK %u %31s", &track_num, mode) != 2 ||
                track_num == 0u || track_num > 99u ||
                !cue_parse_track_layout(mode,
                                        &specs[track_count].raw_sector_size,
                                        &specs[track_count].data_offset,
                                        &specs[track_count].is_audio)) {
                fclose(cue_fp);
                return ODFS_ERR_UNSUPPORTED;
            }
            specs[track_count].number = (uint8_t)track_num;
            track_count++;
            continue;
        }

        if (strncmp(trimmed, "TITLE", 5) == 0 &&
            isspace((unsigned char)trimmed[5])) {
            cue_parse_text_value(trimmed + 6,
                                 track_count != 0u
                                     ? specs[track_count - 1u].title
                                     : disc_title,
                                 HOST_CUE_TEXT_MAX);
            continue;
        }

        if (strncmp(trimmed, "PERFORMER", 9) == 0 &&
            isspace((unsigned char)trimmed[9])) {
            cue_parse_text_value(trimmed + 10,
                                 track_count != 0u
                                     ? specs[track_count - 1u].performer
                                     : disc_performer,
                                 HOST_CUE_TEXT_MAX);
            continue;
        }

        if (strncmp(trimmed, "INDEX 01", 8) == 0 &&
            (trimmed[8] == '\0' || isspace((unsigned char)trimmed[8]))) {
            uint32_t frames;

            if (track_count == 0u) {
                fclose(cue_fp);
                return ODFS_ERR_BAD_FORMAT;
            }
            if (!cue_parse_msf(trimmed + 8, &frames)) {
                fclose(cue_fp);
                return ODFS_ERR_BAD_FORMAT;
            }
            specs[track_count - 1u].start_lba = frames;
            specs[track_count - 1u].have_index = 1;
        }
    }
    fclose(cue_fp);

    if (track_count == 0u || first_file[0] == '\0')
        return ODFS_ERR_BAD_FORMAT;

    for (i = 0; i < track_count; i++) {
        if (!specs[i].have_index)
            return ODFS_ERR_BAD_FORMAT;
    }

    bin_path = host_join_relative_path(path, first_file);
    if (!bin_path)
        return ODFS_ERR_NOMEM;

    fm = calloc(1, sizeof(*fm));
    if (!fm) {
        free(bin_path);
        return ODFS_ERR_NOMEM;
    }

    fm->fp = fopen(bin_path, "rb");
    free(bin_path);
    if (!fm->fp) {
        free(fm);
        return ODFS_ERR_IO;
    }

    if (fseek(fm->fp, 0, SEEK_END) != 0) {
        fclose(fm->fp);
        free(fm);
        return ODFS_ERR_IO;
    }
    file_size = ftell(fm->fp);
    if (file_size < 0) {
        fclose(fm->fp);
        free(fm);
        return ODFS_ERR_IO;
    }

    payload_size = (uint64_t)file_size;
    if (host_riff_data_range(fm->fp, &payload_base, &payload_size)) {
        /* clamp a data chunk that claims more than the file holds */
        if (payload_base + payload_size > (uint64_t)file_size)
            payload_size = (uint64_t)file_size - payload_base;
    } else {
        payload_base = 0;
    }
    rewind(fm->fp);

    fm->is_cue = 1;
    fm->track_count = track_count;
    memcpy(fm->disc_title, disc_title, sizeof(fm->disc_title));
    memcpy(fm->disc_performer, disc_performer, sizeof(fm->disc_performer));

    for (i = 0; i < track_count; i++) {
        uint64_t start_offset = payload_base +
                                (uint64_t)specs[i].start_lba *
                                (uint64_t)specs[i].raw_sector_size;
        uint64_t end_offset;

        if (i + 1u < track_count) {
            if (specs[i + 1u].raw_sector_size != specs[i].raw_sector_size) {
                fclose(fm->fp);
                free(fm);
                return ODFS_ERR_UNSUPPORTED;
            }
            end_offset = payload_base +
                         (uint64_t)specs[i + 1u].start_lba *
                         (uint64_t)specs[i].raw_sector_size;
        } else {
            end_offset = payload_base + payload_size;
        }

        if (end_offset < start_offset ||
            ((end_offset - start_offset) % specs[i].raw_sector_size) != 0u) {
            fclose(fm->fp);
            free(fm);
            return ODFS_ERR_BAD_FORMAT;
        }

        fm->tracks[i].start_lba = specs[i].start_lba;
        fm->tracks[i].sector_count = (uint32_t)((end_offset - start_offset) /
                                   (uint64_t)specs[i].raw_sector_size);
        fm->tracks[i].raw_sector_size = specs[i].raw_sector_size;
        fm->tracks[i].data_offset = specs[i].data_offset;
        fm->tracks[i].file_offset = start_offset;
        fm->tracks[i].number = specs[i].number;
        fm->tracks[i].is_audio = specs[i].is_audio;
        memcpy(fm->tracks[i].title, specs[i].title,
               sizeof(fm->tracks[i].title));
        memcpy(fm->tracks[i].performer, specs[i].performer,
               sizeof(fm->tracks[i].performer));
        if (specs[i].is_audio)
            fm->has_audio = 1;

        if (fm->tracks[i].start_lba + fm->tracks[i].sector_count > sector_count)
            sector_count = fm->tracks[i].start_lba + fm->tracks[i].sector_count;
    }

    fm->sector_count = sector_count;
    out->ops = &file_media_ops;
    out->ctx = fm;
    return ODFS_OK;
}

static odfs_err_t file_read_sectors(void *ctx, uint32_t lba,
                                     uint32_t count, void *buf)
{
    file_media_ctx_t *fm = ctx;
    long offset = (long)lba * HOST_SECTOR_SIZE;

    if (!fm->is_cue) {
        size_t total = (size_t)count * HOST_SECTOR_SIZE;
        size_t got;

        if (fseek(fm->fp, offset, SEEK_SET) != 0)
            return ODFS_ERR_IO;

        got = fread(buf, 1, total, fm->fp);
        if (got != total) {
            if (feof(fm->fp))
                return ODFS_ERR_EOF;
            return ODFS_ERR_IO;
        }

        return ODFS_OK;
    }

    while (count != 0u) {
        cue_track_t *track = NULL;
        size_t i;
        uint8_t sector_buf[2352];
        uint8_t *out = buf;
        uint32_t sector_in_track;
        uint64_t raw_offset;

        if (lba >= fm->sector_count)
            return ODFS_ERR_EOF;

        for (i = 0; i < fm->track_count; i++) {
            uint32_t track_end = fm->tracks[i].start_lba + fm->tracks[i].sector_count;
            if (lba >= fm->tracks[i].start_lba && lba < track_end) {
                track = &fm->tracks[i];
                break;
            }
        }
        if (!track)
            return ODFS_ERR_EOF;

        sector_in_track = lba - track->start_lba;
        raw_offset = track->file_offset +
                     (uint64_t)sector_in_track * (uint64_t)track->raw_sector_size;

        if (track->raw_sector_size == HOST_SECTOR_SIZE && track->data_offset == 0u) {
            if (fseek(fm->fp, (long)raw_offset, SEEK_SET) != 0)
                return ODFS_ERR_IO;
            if (fread(out, 1, HOST_SECTOR_SIZE, fm->fp) != HOST_SECTOR_SIZE)
                return feof(fm->fp) ? ODFS_ERR_EOF : ODFS_ERR_IO;
        } else {
            if (track->raw_sector_size > sizeof(sector_buf))
                return ODFS_ERR_UNSUPPORTED;
            if (fseek(fm->fp, (long)raw_offset, SEEK_SET) != 0)
                return ODFS_ERR_IO;
            if (fread(sector_buf, 1, track->raw_sector_size, fm->fp) !=
                track->raw_sector_size)
                return feof(fm->fp) ? ODFS_ERR_EOF : ODFS_ERR_IO;
            memcpy(out, sector_buf + track->data_offset, HOST_SECTOR_SIZE);
        }

        buf = out + HOST_SECTOR_SIZE;
        lba++;
        count--;
    }

    return ODFS_OK;
}

static odfs_err_t file_read_toc(void *ctx, odfs_toc_t *toc)
{
    file_media_ctx_t *fm = ctx;
    size_t i;

    if (!fm->is_cue || fm->track_count == 0u)
        return ODFS_ERR_UNSUPPORTED;

    memset(toc, 0, sizeof(*toc));
    toc->first_session = 1;
    toc->last_session = 1;
    toc->session_count = (uint8_t)fm->track_count;
    toc->leadout_lba = fm->sector_count;

    for (i = 0; i < fm->track_count; i++) {
        toc->sessions[i].number = fm->tracks[i].number;
        toc->sessions[i].control = fm->tracks[i].is_audio ? 0x00 : 0x04;
        toc->sessions[i].start_lba = fm->tracks[i].start_lba;
        toc->sessions[i].length = fm->tracks[i].sector_count;
    }
    return ODFS_OK;
}

static odfs_err_t file_read_audio(void *ctx, uint32_t lba,
                                  uint32_t count, void *buf)
{
    file_media_ctx_t *fm = ctx;
    uint8_t *out = buf;

    if (!fm->is_cue)
        return ODFS_ERR_UNSUPPORTED;

    while (count != 0u) {
        cue_track_t *track = NULL;
        uint64_t raw_offset;
        size_t i;

        for (i = 0; i < fm->track_count; i++) {
            uint32_t track_end = fm->tracks[i].start_lba +
                                 fm->tracks[i].sector_count;
            if (lba >= fm->tracks[i].start_lba && lba < track_end) {
                track = &fm->tracks[i];
                break;
            }
        }
        if (!track)
            return ODFS_ERR_EOF;
        if (track->raw_sector_size != HOST_AUDIO_FRAME_SIZE)
            return ODFS_ERR_UNSUPPORTED;

        raw_offset = track->file_offset +
                     (uint64_t)(lba - track->start_lba) *
                     HOST_AUDIO_FRAME_SIZE;
        if (fseek(fm->fp, (long)raw_offset, SEEK_SET) != 0)
            return ODFS_ERR_IO;
        if (fread(out, 1, HOST_AUDIO_FRAME_SIZE, fm->fp) !=
            HOST_AUDIO_FRAME_SIZE)
            return feof(fm->fp) ? ODFS_ERR_EOF : ODFS_ERR_IO;

        out += HOST_AUDIO_FRAME_SIZE;
        lba++;
        count--;
    }
    return ODFS_OK;
}

/*
 * Synthesize CD-Text packs from the cue sheet's TITLE/PERFORMER lines
 * in the shape of a READ TOC/PMA/ATIP format 0x05 response, so image
 * files exercise the same pack decoder as a real drive.
 */

static uint16_t cdtext_crc16(const uint8_t *p, size_t n)
{
    uint16_t crc = 0;

    while (n--) {
        int b;

        crc ^= (uint16_t)(*p++ << 8);
        for (b = 0; b < 8; b++)
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u)
                                  : (uint16_t)(crc << 1);
    }
    return (uint16_t)~crc;
}

typedef struct cdtext_writer {
    uint8_t *buf;         /* full response, packs start at offset 4 */
    size_t   packs;
    uint8_t  seq;
    uint8_t  type;
    uint8_t  first_track; /* track owning the pack's first character */
    uint8_t  first_pos;   /* its position within that track's string */
    uint8_t  data[12];
    int      fill;
} cdtext_writer_t;

static void cdtext_writer_flush(cdtext_writer_t *w)
{
    uint8_t *pack;
    uint16_t crc;

    if (w->fill == 0)
        return;
    while (w->fill < 12)
        w->data[w->fill++] = 0;

    pack = w->buf + 4u + w->packs * 18u;
    pack[0] = w->type;
    pack[1] = w->first_track;
    pack[2] = w->seq++;
    pack[3] = w->first_pos; /* block 0, single-byte characters */
    memcpy(pack + 4, w->data, 12u);
    crc = cdtext_crc16(pack, 16u);
    pack[16] = (uint8_t)(crc >> 8);
    pack[17] = (uint8_t)crc;
    w->packs++;
    w->fill = 0;
}

static void cdtext_writer_put(cdtext_writer_t *w, uint8_t type,
                              uint8_t track, const char *s)
{
    size_t len = strlen(s);
    size_t pos;

    /* strings flow across packs, NUL-terminated like on a real disc */
    for (pos = 0; pos <= len; pos++) {
        if (w->fill == 0) {
            w->type = type;
            w->first_track = track;
            w->first_pos = (uint8_t)(pos < 15u ? pos : 15u);
        }
        w->data[w->fill++] = (uint8_t)s[pos];
        if (w->fill == 12)
            cdtext_writer_flush(w);
    }
}

static odfs_err_t file_read_cdtext(void *ctx, uint8_t **buf_out,
                                   size_t *len_out)
{
    file_media_ctx_t *fm = ctx;
    cdtext_writer_t w;
    uint8_t *buf;
    size_t title_bytes;
    size_t performer_bytes;
    size_t pack_cap = 0;
    size_t total;
    size_t i;
    int have_title = fm->disc_title[0] != '\0';
    int have_performer = fm->disc_performer[0] != '\0';

    if (!fm->is_cue)
        return ODFS_ERR_UNSUPPORTED;

    title_bytes = strlen(fm->disc_title) + 1u;
    performer_bytes = strlen(fm->disc_performer) + 1u;
    for (i = 0; i < fm->track_count; i++) {
        if (fm->tracks[i].title[0] != '\0')
            have_title = 1;
        if (fm->tracks[i].performer[0] != '\0')
            have_performer = 1;
        title_bytes += strlen(fm->tracks[i].title) + 1u;
        performer_bytes += strlen(fm->tracks[i].performer) + 1u;
    }
    if (!have_title && !have_performer)
        return ODFS_ERR_UNSUPPORTED;

    if (have_title)
        pack_cap += (title_bytes + 11u) / 12u;
    if (have_performer)
        pack_cap += (performer_bytes + 11u) / 12u;

    buf = malloc(4u + pack_cap * 18u);
    if (!buf)
        return ODFS_ERR_NOMEM;

    memset(&w, 0, sizeof(w));
    w.buf = buf;

    if (have_title) {
        cdtext_writer_put(&w, 0x80, 0, fm->disc_title);
        for (i = 0; i < fm->track_count; i++)
            cdtext_writer_put(&w, 0x80, fm->tracks[i].number,
                              fm->tracks[i].title);
        cdtext_writer_flush(&w);
    }
    if (have_performer) {
        cdtext_writer_put(&w, 0x81, 0, fm->disc_performer);
        for (i = 0; i < fm->track_count; i++)
            cdtext_writer_put(&w, 0x81, fm->tracks[i].number,
                              fm->tracks[i].performer);
        cdtext_writer_flush(&w);
    }

    total = 4u + w.packs * 18u;
    buf[0] = (uint8_t)((total - 2u) >> 8);
    buf[1] = (uint8_t)(total - 2u);
    buf[2] = 0;
    buf[3] = 0;

    *buf_out = buf;
    *len_out = total;
    return ODFS_OK;
}

static uint32_t file_sector_size(void *ctx)
{
    (void)ctx;
    return HOST_SECTOR_SIZE;
}

static uint32_t file_sector_count(void *ctx)
{
    file_media_ctx_t *fm = ctx;
    return fm->sector_count;
}

static void file_close(void *ctx)
{
    file_media_ctx_t *fm = ctx;
    if (fm) {
        if (fm->fp)
            fclose(fm->fp);
        free(fm);
    }
}

static const odfs_media_ops_t file_media_ops = {
    .read_sectors          = file_read_sectors,
    .sector_size           = file_sector_size,
    .sector_count          = file_sector_count,
    .read_toc              = file_read_toc,
    .read_last_session_lba = NULL,
    .read_audio            = file_read_audio,
    .read_cdtext           = file_read_cdtext,
    .close                 = file_close,
};

odfs_err_t odfs_media_open_image(const char *path, odfs_media_t *out)
{
    file_media_ctx_t *fm;
    long file_size;

    if (!path || !out)
        return ODFS_ERR_INVAL;

    if (host_ext_eq(path, ".cue"))
        return cue_media_open(path, out);

    fm = calloc(1, sizeof(*fm));
    if (!fm)
        return ODFS_ERR_NOMEM;

    fm->fp = fopen(path, "rb");
    if (!fm->fp) {
        free(fm);
        return ODFS_ERR_IO;
    }

    /* determine size */
    if (fseek(fm->fp, 0, SEEK_END) != 0) {
        fclose(fm->fp);
        free(fm);
        return ODFS_ERR_IO;
    }
    file_size = ftell(fm->fp);
    if (file_size < 0) {
        fclose(fm->fp);
        free(fm);
        return ODFS_ERR_IO;
    }
    rewind(fm->fp);

    fm->sector_count = (uint32_t)(file_size / HOST_SECTOR_SIZE);

    out->ops = &file_media_ops;
    out->ctx = fm;
    return ODFS_OK;
}
