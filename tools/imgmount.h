/*
 * imgmount.h — mount an image with the Amiga handler's CDDA fallback
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef ODFS_TOOLS_IMGMOUNT_H
#define ODFS_TOOLS_IMGMOUNT_H

#include "odfs/api.h"

#if ODFS_FEATURE_CDDA
#include "cdda/cdda.h"
#include <string.h>
#endif

/*
 * Mount like the Amiga handler: probe the data filesystems first and
 * fall back to a pure audio CD via the CDDA backend when none is
 * found. On a successful data mount the CDDA backend is additionally
 * registered so the CDDA/ directory of mixed-mode discs resolves.
 */
static odfs_err_t img_mount_image(odfs_media_t *media,
                                  const odfs_mount_opts_t *opts,
                                  odfs_log_state_t *log,
                                  odfs_mount_t *mnt)
{
    odfs_err_t err = odfs_mount(media, opts, log, mnt);

#if ODFS_FEATURE_CDDA
    odfs_toc_t toc;
    odfs_node_t cdda_root;
    void *cdda_ctx;

    if (err != ODFS_OK) {
        if (odfs_media_read_toc(media, &toc) != ODFS_OK)
            return err;
        if (cdda_mount_from_toc(&toc, 0, opts, media,
                                &cdda_root, &cdda_ctx) != ODFS_OK)
            return err;

        memset(mnt, 0, sizeof(*mnt));
        mnt->media = *media;
        mnt->log = *log;
        mnt->opts = *opts;
        mnt->root = cdda_root;
        mnt->backend_ops = &cdda_backend_ops;
        mnt->backend_ctx = cdda_ctx;
        mnt->active_backend = ODFS_BACKEND_CDDA;
        odfs_mount_register_backend(mnt, ODFS_BACKEND_CDDA,
                                    &cdda_backend_ops, cdda_ctx,
                                    &cdda_root);
        (void)cdda_backend_ops.get_volume_name(cdda_ctx, mnt->volume_name,
                                               sizeof(mnt->volume_name));
        return ODFS_OK;
    }

    if (odfs_media_read_toc(media, &toc) == ODFS_OK &&
        cdda_mount_from_toc(&toc, 1, opts, media,
                            &cdda_root, &cdda_ctx) == ODFS_OK)
        odfs_mount_register_backend(mnt, ODFS_BACKEND_CDDA,
                                    &cdda_backend_ops, cdda_ctx,
                                    &cdda_root);
#endif

    return err;
}

#endif /* ODFS_TOOLS_IMGMOUNT_H */
