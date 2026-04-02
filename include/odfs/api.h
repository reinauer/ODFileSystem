/*
 * odfs/api.h — top-level public API
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef ODFS_API_H
#define ODFS_API_H

#include "odfs/config.h"
#include "odfs/error.h"
#include "odfs/node.h"
#include "odfs/media.h"
#include "odfs/cache.h"
#include "odfs/log.h"
#include "odfs/backend.h"

/* mount options */
typedef struct odfs_mount_opts {
    int      force_backend;      /* ODFS_BACKEND_xxx or 0 for auto */
    int      force_session;      /* session number, -1 for default (last) */
    int      disable_rr;         /* disable Rock Ridge */
    int      disable_joliet;     /* disable Joliet */
    int      prefer_udf;         /* prefer UDF over ISO on bridge discs */
    int      prefer_hfs;         /* prefer HFS on hybrid discs */
    int      lowercase_iso;      /* lowercase plain ISO names */
    uint32_t cache_blocks;       /* block cache size (0 = use default) */
} odfs_mount_opts_t;

/* mounted volume */
typedef struct odfs_mount {
    odfs_media_t             media;
    odfs_cache_t             cache;
    odfs_log_state_t         log;
    odfs_mount_opts_t        opts;

    odfs_backend_type_t      active_backend;
    const odfs_backend_ops_t *backend_ops;
    void                      *backend_ctx;

    odfs_node_t              root;
    char                      volume_name[128];
} odfs_mount_t;

/* initialize default mount options */
void odfs_mount_opts_default(odfs_mount_opts_t *opts);

/* mount an image/device */
odfs_err_t odfs_mount(odfs_media_t *media,
                        const odfs_mount_opts_t *opts,
                        odfs_log_state_t *log,
                        odfs_mount_t *mnt);

/* unmount */
void odfs_unmount(odfs_mount_t *mnt);

/* directory listing (resume_offset: NULL = from start, else resume point) */
odfs_err_t odfs_readdir(odfs_mount_t *mnt,
                          const odfs_node_t *dir,
                          odfs_dir_iter_fn callback,
                          void *ctx,
                          uint32_t *resume_offset);

/* file read */
odfs_err_t odfs_read(odfs_mount_t *mnt,
                       const odfs_node_t *file,
                       uint64_t offset,
                       void *buf,
                       size_t *len);

/* lookup path component */
odfs_err_t odfs_lookup(odfs_mount_t *mnt,
                         const odfs_node_t *dir,
                         const char *name,
                         odfs_node_t *out);

/* resolve full path from root */
odfs_err_t odfs_resolve_path(odfs_mount_t *mnt,
                               const char *path,
                               odfs_node_t *out);

#endif /* ODFS_API_H */
