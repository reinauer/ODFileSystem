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

typedef struct cue_track {
    uint32_t start_lba;
    uint32_t sector_count;
    uint32_t raw_sector_size;
    uint32_t data_offset;
    uint64_t file_offset;
} cue_track_t;

typedef struct file_media_ctx {
    FILE       *fp;
    uint32_t    sector_count;
    int         is_cue;
    size_t      track_count;
    cue_track_t tracks[HOST_MAX_CUE_TRACKS];
} file_media_ctx_t;

typedef struct cue_track_spec {
    uint32_t start_lba;
    uint32_t raw_sector_size;
    uint32_t data_offset;
    int      have_index;
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
                                  uint32_t *data_offset)
{
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

static odfs_err_t cue_media_open(const char *path, odfs_media_t *out)
{
    char line[512];
    char current_file[512];
    char first_file[512];
    cue_track_spec_t specs[HOST_MAX_CUE_TRACKS];
    file_media_ctx_t *fm;
    FILE *cue_fp;
    char *bin_path = NULL;
    size_t track_count = 0;
    size_t i;
    long file_size;
    uint32_t sector_count = 0;

    memset(specs, 0, sizeof(specs));
    current_file[0] = '\0';
    first_file[0] = '\0';

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
                !cue_parse_track_layout(mode,
                                        &specs[track_count].raw_sector_size,
                                        &specs[track_count].data_offset)) {
                fclose(cue_fp);
                return ODFS_ERR_UNSUPPORTED;
            }
            (void)track_num;
            track_count++;
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
    rewind(fm->fp);

    fm->is_cue = 1;
    fm->track_count = track_count;

    for (i = 0; i < track_count; i++) {
        uint64_t start_offset = (uint64_t)specs[i].start_lba *
                                (uint64_t)specs[i].raw_sector_size;
        uint64_t end_offset;

        if (i + 1u < track_count) {
            if (specs[i + 1u].raw_sector_size != specs[i].raw_sector_size) {
                fclose(fm->fp);
                free(fm);
                return ODFS_ERR_UNSUPPORTED;
            }
            end_offset = (uint64_t)specs[i + 1u].start_lba *
                         (uint64_t)specs[i].raw_sector_size;
        } else {
            end_offset = (uint64_t)file_size;
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
    .read_toc              = NULL,
    .read_last_session_lba = NULL,
    .read_audio            = NULL,
    .read_cdtext           = NULL,
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
