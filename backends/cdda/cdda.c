/*
 * cdda.c — CDDA virtual file backend
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Presents audio CD tracks as virtual WAV files. The WAV header is
 * synthesized on-the-fly; audio data is read via the media layer
 * (SCSI Read CD on Amiga hardware).
 *
 * On mixed-mode discs (data + audio), tracks appear in a virtual
 * CDDA/ subdirectory so they don't mix with data files. On pure
 * audio CDs, tracks appear at the root.
 *
 * Track detection requires a TOC from the media layer. On host
 * images without TOC support, the backend will not activate.
 */

#include "cdda.h"
#include "odfs/cache.h"
#include "odfs/log.h"
#include "odfs/error.h"

#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

/* ------------------------------------------------------------------ */
/* WAV header generation                                               */
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

/* ------------------------------------------------------------------ */
/* node IDs and encoding                                               */
/* ------------------------------------------------------------------ */

/*
 * Node ID encoding for CDDA:
 *   0          = root (or CDDA/ virtual dir on mixed-mode)
 *   1..99      = track number
 *   extent.lba = track index into tracks[] array (for quick lookup)
 */

static void cdda_track_name(int track_num, char *buf, size_t buf_size)
{
    /* "Track01.wav" .. "Track99.wav" */
    int len = 0;
    buf[len++] = 'T';
    buf[len++] = 'r';
    buf[len++] = 'a';
    buf[len++] = 'c';
    buf[len++] = 'k';
    buf[len++] = '0' + (track_num / 10);
    buf[len++] = '0' + (track_num % 10);
    buf[len++] = '.';
    buf[len++] = 'w';
    buf[len++] = 'a';
    buf[len++] = 'v';
    buf[len] = '\0';
    (void)buf_size;
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
                                 odfs_node_t *root_out,
                                 void **backend_ctx)
{
    cdda_context_t *ctx;
    int audio_count = 0;

    ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return ODFS_ERR_NOMEM;

    ctx->is_mixed_mode = has_data_session;

    /* identify audio tracks from TOC */
    for (int i = 0; i < toc->session_count && i + 1 < toc->session_count; i++) {
        /* simple heuristic: audio tracks have control field bit 2 = 0
         * In our simplified TOC, we don't have the control field yet,
         * so we treat all non-data tracks as audio.
         * For now, populate from TOC entries. */
    }

    /*
     * Since our TOC doesn't distinguish data vs audio yet,
     * populate tracks from all TOC entries as a placeholder.
     * On real hardware with SCSI Read TOC, the control byte
     * (bit 2) distinguishes data (1) from audio (0).
     */
    for (int i = 0; i < toc->session_count && audio_count < CDDA_MAX_TRACKS; i++) {
        uint32_t start = toc->sessions[i].start_lba;
        uint32_t length = toc->sessions[i].length;

        if (length == 0 && i + 1 < toc->session_count)
            length = toc->sessions[i + 1].start_lba - start;

        if (length == 0)
            continue;

        ctx->tracks[audio_count].number = toc->sessions[i].number;
        ctx->tracks[audio_count].start_lba = start;
        ctx->tracks[audio_count].length_frames = length;
        ctx->tracks[audio_count].data_size = (uint64_t)length * CDDA_FRAME_SIZE;
        ctx->tracks[audio_count].wav_size = ctx->tracks[audio_count].data_size
                                            + CDDA_WAV_HEADER_SIZE;
        audio_count++;
    }

    ctx->track_count = audio_count;

    if (audio_count == 0) {
        free(ctx);
        return ODFS_ERR_BAD_FORMAT;
    }

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
    free(backend_ctx);
}

/* ------------------------------------------------------------------ */
/* readdir                                                             */
/* ------------------------------------------------------------------ */

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

    for (int i = start; i < ctx->track_count; i++) {
        odfs_node_t node;
        memset(&node, 0, sizeof(node));

        node.id = ctx->tracks[i].number;
        node.parent_id = 0;
        node.backend = ODFS_BACKEND_CDDA;
        node.kind = ODFS_NODE_VIRTUAL;
        node.size = ctx->tracks[i].wav_size;
        node.extent.lba = i; /* track index */
        node.extent.length = (uint32_t)ctx->tracks[i].wav_size;

        cdda_track_name(ctx->tracks[i].number, node.name, sizeof(node.name));

        odfs_err_t err = callback(&node, cb_ctx);
        if (err != ODFS_OK) {
            if (resume_offset)
                *resume_offset = (uint32_t)(i + 1);
            return err;
        }
    }

    if (resume_offset)
        *resume_offset = (uint32_t)ctx->track_count;
    return ODFS_OK;
}

/* ------------------------------------------------------------------ */
/* read — synthesize WAV header + read audio frames                    */
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

    int track_idx = (int)file->extent.lba;
    if (track_idx < 0 || track_idx >= ctx->track_count) {
        *len = 0;
        return ODFS_ERR_NOT_FOUND;
    }

    cdda_track_t *trk = &ctx->tracks[track_idx];
    size_t want = *len;
    size_t done = 0;
    uint8_t *out = buf;

    if (offset >= trk->wav_size) {
        *len = 0;
        return ODFS_OK;
    }
    if (offset + want > trk->wav_size)
        want = (size_t)(trk->wav_size - offset);

    /* serve WAV header bytes if offset is within header */
    if (offset < CDDA_WAV_HEADER_SIZE) {
        uint8_t hdr[CDDA_WAV_HEADER_SIZE];
        cdda_build_wav_header(hdr, (uint32_t)trk->data_size);

        size_t hdr_avail = CDDA_WAV_HEADER_SIZE - (size_t)offset;
        size_t chunk = (hdr_avail < want) ? hdr_avail : want;
        memcpy(out, hdr + offset, chunk);
        done += chunk;
    }

    /*
     * Audio data beyond the header would be read via SCSI Read CD.
     * On host (no SCSI), we return zeros as placeholder.
     *
     * TODO: on Amiga, issue SCSI Read CD (0xBE) to read raw audio
     * frames from the disc. The frame offset is:
     *   audio_offset = (offset - CDDA_WAV_HEADER_SIZE)
     *   frame_num = audio_offset / CDDA_FRAME_SIZE
     *   start_frame = trk->start_lba + frame_num
     */
    if (done < want) {
        /* fill remaining with silence (host placeholder) */
        memset(out + done, 0, want - done);
        done = want;
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

    for (int i = 0; i < ctx->track_count; i++) {
        char tname[32];
        cdda_track_name(ctx->tracks[i].number, tname, sizeof(tname));
        if (strcasecmp(name, tname) == 0) {
            memset(out, 0, sizeof(*out));
            out->id = ctx->tracks[i].number;
            out->parent_id = 0;
            out->backend = ODFS_BACKEND_CDDA;
            out->kind = ODFS_NODE_VIRTUAL;
            out->size = ctx->tracks[i].wav_size;
            out->extent.lba = i;
            out->extent.length = (uint32_t)ctx->tracks[i].wav_size;
            memcpy(out->name, tname, strlen(tname) + 1);
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
    const char *name = ctx->is_mixed_mode ? "CDDA" : "Audio CD";
    size_t len = strlen(name);
    if (len >= buf_size) len = buf_size - 1;
    memcpy(buf, name, len);
    buf[len] = '\0';
    return ODFS_OK;
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
};
