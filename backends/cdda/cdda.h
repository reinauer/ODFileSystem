/*
 * cdda.h — CDDA (audio CD) virtual file backend
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Exposes audio CD tracks as virtual WAV or AIFF files. On mixed-mode
 * discs, tracks appear in a virtual CDDA/ subdirectory. On pure audio
 * CDs, tracks appear at the root.
 */

#ifndef ODFS_CDDA_H
#define ODFS_CDDA_H

#include "odfs/api.h"
#include <stdint.h>

/* CD audio constants */
#define CDDA_SAMPLE_RATE     44100
#define CDDA_CHANNELS        2
#define CDDA_BITS_PER_SAMPLE 16
#define CDDA_FRAME_SIZE      2352   /* bytes per CD audio frame */
#define CDDA_FRAMES_PER_SEC  75     /* CD frames per second */
#define CDDA_WAV_HEADER_SIZE 44     /* PCM WAV header */
#define CDDA_AIFF_HEADER_SIZE 54    /* AIFF COMM + SSND header */

#define CDDA_MAX_TRACKS      99

typedef enum cdda_file_format {
    CDDA_FILE_FORMAT_WAV = 0,
    CDDA_FILE_FORMAT_AIFF = 1
} cdda_file_format_t;

/* track info */
typedef struct cdda_track {
    uint8_t  number;
    uint32_t start_lba;    /* first frame of track */
    uint32_t length_frames; /* number of frames */
    uint64_t data_size;    /* PCM data size in bytes */
    uint64_t file_size;    /* total file size (header + data) */
} cdda_track_t;

/* CDDA mount context */
typedef struct cdda_context {
    int                track_count;
    cdda_track_t       tracks[CDDA_MAX_TRACKS];
    int                is_mixed_mode;   /* has both data and audio tracks */
    cdda_file_format_t file_format;
    char               *cddb_text;
    size_t             cddb_size;
    char               *cdtext_text;
    size_t             cdtext_size;
    odfs_media_t       *media;          /* for read_audio calls */
} cdda_context_t;

extern const odfs_backend_ops_t cdda_backend_ops;

/* mount directly from a TOC (not through standard probe path) */
odfs_err_t cdda_mount_from_toc(const odfs_toc_t *toc,
                               int has_data_session,
                               const odfs_mount_opts_t *opts,
                               odfs_media_t *media,
                               odfs_node_t *root_out,
                               void **backend_ctx);

#endif /* ODFS_CDDA_H */
