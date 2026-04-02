/*
 * mount.c — mount/unmount logic
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "odfs/api.h"
#include <string.h>
#include <inttypes.h>

/* backend registrations */
#if ODFS_FEATURE_ISO9660
extern const odfs_backend_ops_t iso9660_backend_ops;
#endif

/* ordered backend probe table (highest priority first) */
static const odfs_backend_ops_t *backend_table[] = {
#if ODFS_FEATURE_ISO9660
    &iso9660_backend_ops,
#endif
    NULL
};

void odfs_mount_opts_default(odfs_mount_opts_t *opts)
{
    memset(opts, 0, sizeof(*opts));
    opts->force_backend = 0;
    opts->force_session = -1;
    opts->disable_rr = 0;
    opts->disable_joliet = 0;
    opts->prefer_udf = 0;
    opts->prefer_hfs = 0;
    opts->lowercase_iso = 0; /* preserve original case */
    opts->cache_blocks = 0; /* use default from config.h */
}

odfs_err_t odfs_mount(odfs_media_t *media,
                        const odfs_mount_opts_t *opts,
                        odfs_log_state_t *log,
                        odfs_mount_t *mnt)
{
    odfs_err_t err;
    uint32_t cache_size;

    if (!media || !mnt)
        return ODFS_ERR_INVAL;

    memset(mnt, 0, sizeof(*mnt));
    mnt->media = *media;

    if (opts)
        mnt->opts = *opts;
    else
        odfs_mount_opts_default(&mnt->opts);

    if (log)
        mnt->log = *log;
    else
        odfs_log_init(&mnt->log);

    /* init block cache */
    cache_size = mnt->opts.cache_blocks;
    if (cache_size == 0)
        cache_size = ODFS_BLOCK_CACHE_SIZE;

    err = odfs_cache_init(&mnt->cache, &mnt->media, cache_size);
    if (err != ODFS_OK)
        return err;

    ODFS_INFO(&mnt->log, ODFS_SUB_MOUNT,
               "cache initialized: %" PRIu32 " blocks", cache_size);

    /* determine session start (default: 0) */
    uint32_t session_start = 0;
    /* TODO: session discovery from TOC when force_session != -1 */

    /* probe backends in priority order */
    const odfs_backend_ops_t *chosen = NULL;
    for (int i = 0; backend_table[i] != NULL; i++) {
        const odfs_backend_ops_t *be = backend_table[i];

        /* skip if user forced a different backend */
        if (mnt->opts.force_backend != 0) {
            /* match by type enum — for now just ISO9660 */
            if (mnt->opts.force_backend == ODFS_BACKEND_ISO9660 &&
                be != &iso9660_backend_ops)
                continue;
        }

        ODFS_DEBUG(&mnt->log, ODFS_SUB_MOUNT,
                    "probing backend: %s", be->name);

        err = be->probe(&mnt->cache, &mnt->log, session_start);
        if (err == ODFS_OK) {
            chosen = be;
            ODFS_INFO(&mnt->log, ODFS_SUB_MOUNT,
                       "detected format: %s", be->name);
            break;
        }
    }

    if (!chosen) {
        ODFS_WARN(&mnt->log, ODFS_SUB_MOUNT,
                   "no recognized filesystem format found");
        odfs_cache_destroy(&mnt->cache);
        return ODFS_ERR_BAD_FORMAT;
    }

    /* mount with selected backend */
    err = chosen->mount(&mnt->cache, &mnt->log, session_start,
                        &mnt->root, &mnt->backend_ctx);
    if (err != ODFS_OK) {
        ODFS_ERROR(&mnt->log, ODFS_SUB_MOUNT,
                    "backend %s mount failed: %s", chosen->name, odfs_err_str(err));
        odfs_cache_destroy(&mnt->cache);
        return err;
    }

    mnt->backend_ops = chosen;
    mnt->active_backend = chosen->backend_type;

    /* retrieve volume name from backend */
    if (chosen->get_volume_name)
        chosen->get_volume_name(mnt->backend_ctx,
                                mnt->volume_name, sizeof(mnt->volume_name));

    return ODFS_OK;
}

void odfs_unmount(odfs_mount_t *mnt)
{
    if (!mnt)
        return;

    if (mnt->backend_ops && mnt->backend_ops->unmount)
        mnt->backend_ops->unmount(mnt->backend_ctx);

    odfs_cache_destroy(&mnt->cache);
    odfs_media_close(&mnt->media);

    memset(mnt, 0, sizeof(*mnt));
}

odfs_err_t odfs_readdir(odfs_mount_t *mnt,
                          const odfs_node_t *dir,
                          odfs_dir_iter_fn callback,
                          void *ctx)
{
    if (!mnt || !mnt->backend_ops || !mnt->backend_ops->readdir)
        return ODFS_ERR_UNSUPPORTED;

    if (dir->kind != ODFS_NODE_DIR)
        return ODFS_ERR_NOT_DIR;

    return mnt->backend_ops->readdir(mnt->backend_ctx, &mnt->cache,
                                     &mnt->log, dir, callback, ctx);
}

odfs_err_t odfs_read(odfs_mount_t *mnt,
                       const odfs_node_t *file,
                       uint64_t offset,
                       void *buf,
                       size_t *len)
{
    if (!mnt || !mnt->backend_ops || !mnt->backend_ops->read)
        return ODFS_ERR_UNSUPPORTED;

    if (file->kind == ODFS_NODE_DIR)
        return ODFS_ERR_IS_DIR;

    return mnt->backend_ops->read(mnt->backend_ctx, &mnt->cache,
                                  &mnt->log, file, offset, buf, len);
}

odfs_err_t odfs_lookup(odfs_mount_t *mnt,
                         const odfs_node_t *dir,
                         const char *name,
                         odfs_node_t *out)
{
    if (!mnt || !mnt->backend_ops || !mnt->backend_ops->lookup)
        return ODFS_ERR_UNSUPPORTED;

    if (dir->kind != ODFS_NODE_DIR)
        return ODFS_ERR_NOT_DIR;

    return mnt->backend_ops->lookup(mnt->backend_ctx, &mnt->cache,
                                    &mnt->log, dir, name, out);
}

odfs_err_t odfs_resolve_path(odfs_mount_t *mnt,
                               const char *path,
                               odfs_node_t *out)
{
    odfs_node_t current;
    char component[ODFS_NAME_MAX];
    const char *p;
    size_t len;
    odfs_err_t err;

    if (!mnt || !path || !out)
        return ODFS_ERR_INVAL;

    current = mnt->root;

    /* skip leading separator */
    p = path;
    while (*p == '/')
        p++;

    if (*p == '\0') {
        *out = current;
        return ODFS_OK;
    }

    while (*p) {
        /* extract next component */
        const char *slash = p;
        while (*slash && *slash != '/')
            slash++;
        len = (size_t)(slash - p);
        if (len == 0) {
            p = slash + 1;
            continue;
        }
        if (len >= ODFS_NAME_MAX)
            return ODFS_ERR_NAME_TOO_LONG;

        memcpy(component, p, len);
        component[len] = '\0';

        err = odfs_lookup(mnt, &current, component, &current);
        if (err != ODFS_OK)
            return err;

        p = slash;
        while (*p == '/')
            p++;
    }

    *out = current;
    return ODFS_OK;
}
