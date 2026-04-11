/*
 * file_media.c — host-side file-backed media implementation
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "odfs/media.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HOST_SECTOR_SIZE 2048

typedef struct file_media_ctx {
    FILE     *fp;
    uint32_t  sector_count;
} file_media_ctx_t;

static odfs_err_t file_read_sectors(void *ctx, uint32_t lba,
                                     uint32_t count, void *buf)
{
    file_media_ctx_t *fm = ctx;
    long offset = (long)lba * HOST_SECTOR_SIZE;

    if (fseek(fm->fp, offset, SEEK_SET) != 0)
        return ODFS_ERR_IO;

    size_t total = (size_t)count * HOST_SECTOR_SIZE;
    size_t got = fread(buf, 1, total, fm->fp);
    if (got != total) {
        if (feof(fm->fp))
            return ODFS_ERR_EOF;
        return ODFS_ERR_IO;
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
    .read_sectors = file_read_sectors,
    .sector_size  = file_sector_size,
    .sector_count = file_sector_count,
    .read_toc     = NULL,
    .read_audio   = NULL,
    .read_cdtext  = NULL,
    .close        = file_close,
};

odfs_err_t odfs_media_open_image(const char *path, odfs_media_t *out)
{
    file_media_ctx_t *fm;
    long file_size;

    if (!path || !out)
        return ODFS_ERR_INVAL;

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
