/*
 * odfs/media.h — media access abstraction
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef ODFS_MEDIA_H
#define ODFS_MEDIA_H

#include "odfs/error.h"
#include <stdint.h>
#include <stddef.h>

/* TOC track/session entry */
typedef struct odfs_session {
    uint8_t  number;
    uint8_t  control;    /* SCSI TOC control nibble; bit 2 set = data track */
    uint32_t start_lba;
    uint32_t length;     /* in sectors, 0 if unknown */
} odfs_session_t;

/* TOC information */
typedef struct odfs_toc {
    uint8_t         first_session;
    uint8_t         last_session;
    uint8_t         session_count;
    odfs_session_t sessions[99]; /* CD max 99 TOC entries */
} odfs_toc_t;

/* media operations vtable */
typedef struct odfs_media_ops {
    /*
     * Read sectors from media.
     *   lba    — starting logical block address
     *   count  — number of sectors to read
     *   buf    — output buffer (must hold count * sector_size bytes)
     */
    odfs_err_t (*read_sectors)(void *ctx, uint32_t lba, uint32_t count, void *buf);

    /*
     * Get sector size in bytes (typically 2048).
     */
    uint32_t (*sector_size)(void *ctx);

    /*
     * Get total sector count (0 if unknown).
     */
    uint32_t (*sector_count)(void *ctx);

    /*
     * Read TOC/session info.  Returns ODFS_ERR_UNSUPPORTED if not available.
     */
    odfs_err_t (*read_toc)(void *ctx, odfs_toc_t *toc);

    /*
     * Read raw audio CD frames (2352 bytes each).
     * Returns ODFS_ERR_UNSUPPORTED if not available (host images).
     *   lba    — starting frame LBA
     *   count  — number of frames to read
     *   buf    — output buffer (must hold count * 2352 bytes)
     */
    odfs_err_t (*read_audio)(void *ctx, uint32_t lba, uint32_t count, void *buf);

    /*
     * Close / release media.  May be NULL.
     */
    void (*close)(void *ctx);
} odfs_media_ops_t;

/* media handle */
typedef struct odfs_media {
    const odfs_media_ops_t *ops;
    void                    *ctx;    /* backend-private context */
} odfs_media_t;

/* convenience wrappers */
static inline odfs_err_t odfs_media_read(odfs_media_t *m,
                                           uint32_t lba,
                                           uint32_t count,
                                           void *buf)
{
    return m->ops->read_sectors(m->ctx, lba, count, buf);
}

static inline uint32_t odfs_media_sector_size(odfs_media_t *m)
{
    return m->ops->sector_size(m->ctx);
}

static inline uint32_t odfs_media_sector_count(odfs_media_t *m)
{
    return m->ops->sector_count(m->ctx);
}

static inline odfs_err_t odfs_media_read_toc(odfs_media_t *m, odfs_toc_t *toc)
{
    if (!m->ops->read_toc)
        return ODFS_ERR_UNSUPPORTED;
    return m->ops->read_toc(m->ctx, toc);
}

static inline odfs_err_t odfs_media_read_audio(odfs_media_t *m,
                                                  uint32_t lba,
                                                  uint32_t count,
                                                  void *buf)
{
    if (!m->ops->read_audio)
        return ODFS_ERR_UNSUPPORTED;
    return m->ops->read_audio(m->ctx, lba, count, buf);
}

static inline void odfs_media_close(odfs_media_t *m)
{
    if (m->ops->close)
        m->ops->close(m->ctx);
}

/* host-side: open an image file as media */
odfs_err_t odfs_media_open_image(const char *path, odfs_media_t *out);

#endif /* ODFS_MEDIA_H */
