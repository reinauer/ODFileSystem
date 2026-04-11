/*
 * mount.c — mount/unmount logic
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "odfs/api.h"
#include "odfs/string.h"
#include <string.h>
#include <inttypes.h>

/* backend registrations */
#if ODFS_FEATURE_ISO9660
extern const odfs_backend_ops_t iso9660_backend_ops;
#endif
#if ODFS_FEATURE_JOLIET
extern const odfs_backend_ops_t joliet_backend_ops;
#endif
#if ODFS_FEATURE_UDF
extern const odfs_backend_ops_t udf_backend_ops;
#endif
#if ODFS_FEATURE_HFS
extern const odfs_backend_ops_t hfs_backend_ops;
#endif
#if ODFS_FEATURE_HFSPLUS
extern const odfs_backend_ops_t hfsplus_backend_ops;
#endif

/*
 * Backend probe table — order defines precedence.
 *
 * ISO9660 first (RR detected inside mount). Joliet second.
 * UDF and HFS probed independently for standalone media.
 * For hybrid discs, ISO-family wins unless overridden.
 */
static const odfs_backend_ops_t *backend_table[] = {
#if ODFS_FEATURE_ISO9660
    &iso9660_backend_ops,
#endif
#if ODFS_FEATURE_JOLIET
    &joliet_backend_ops,
#endif
#if ODFS_FEATURE_UDF
    &udf_backend_ops,
#endif
#if ODFS_FEATURE_HFS
    &hfs_backend_ops,
#endif
#if ODFS_FEATURE_HFSPLUS
    &hfsplus_backend_ops,
#endif
    NULL
};

static int mount_is_root(const odfs_mount_t *mnt, const odfs_node_t *node)
{
    if (!mnt || !node)
        return 0;

    return node->kind == mnt->root.kind &&
           node->backend == mnt->root.backend &&
           node->id == mnt->root.id &&
           node->extent.lba == mnt->root.extent.lba &&
           node->extent.length == mnt->root.extent.length;
}

static int mount_backend_for_type(const odfs_mount_t *mnt,
                                  odfs_backend_type_t type,
                                  const odfs_backend_ops_t **ops_out,
                                  void **ctx_out)
{
    if (!mnt || type <= ODFS_BACKEND_NONE || type >= ODFS_BACKEND__COUNT)
        return 0;

    if (mnt->backend_map[type]) {
        if (ops_out)
            *ops_out = mnt->backend_map[type];
        if (ctx_out)
            *ctx_out = mnt->backend_ctx_map[type];
        return 1;
    }

    if (mnt->backend_ops &&
        (type == mnt->root.backend || type == mnt->backend_ops->backend_type)) {
        if (ops_out)
            *ops_out = mnt->backend_ops;
        if (ctx_out)
            *ctx_out = mnt->backend_ctx;
        return 1;
    }

    return 0;
}

static int mount_virtual_root_by_name(const odfs_mount_t *mnt,
                                      const odfs_node_t *dir,
                                      const char *name,
                                      odfs_node_t *out)
{
    int i;

    if (!mnt || !dir || !name || !out || !mount_is_root(mnt, dir))
        return 0;

    for (i = ODFS_BACKEND_NONE + 1; i < ODFS_BACKEND__COUNT; i++) {
        if (!mnt->has_virtual_root[i])
            continue;
        if (odfs_strcasecmp(name, mnt->virtual_root_map[i].name) != 0)
            continue;
        *out = mnt->virtual_root_map[i];
        return 1;
    }

    return 0;
}

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

void odfs_mount_register_backend(odfs_mount_t *mnt,
                                   odfs_backend_type_t node_backend,
                                   const odfs_backend_ops_t *ops,
                                   void *ctx,
                                   const odfs_node_t *virtual_root)
{
    if (!mnt || node_backend <= ODFS_BACKEND_NONE ||
        node_backend >= ODFS_BACKEND__COUNT)
        return;

    mnt->backend_map[node_backend] = ops;
    mnt->backend_ctx_map[node_backend] = ctx;
    mnt->has_virtual_root[node_backend] = 0;

    if (virtual_root) {
        mnt->virtual_root_map[node_backend] = *virtual_root;
        mnt->has_virtual_root[node_backend] = 1;
    }
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

    /* determine session start */
    uint32_t session_start = 0;
#if ODFS_FEATURE_MULTISESSION
    if (mnt->opts.force_session >= 0) {
        /* user forced a specific session — read TOC to find it */
        odfs_toc_t toc;
        if (odfs_media_read_toc(&mnt->media, &toc) == ODFS_OK &&
            mnt->opts.force_session < toc.session_count) {
            session_start = toc.sessions[mnt->opts.force_session].start_lba;
            ODFS_INFO(&mnt->log, ODFS_SUB_MOUNT,
                       "forced session %d at LBA %" PRIu32,
                       mnt->opts.force_session, session_start);
        }
    } else {
        /* default: find and use last session */
        odfs_find_last_session(&mnt->media, &mnt->log, &session_start);
    }
#endif

    /*
     * Probe backends and select best match.
     *
     * Precedence (highest first): Rock Ridge > Joliet > plain ISO.
     * ISO9660 is always probed first because RR detection happens
     * inside its mount. If RR is found, we're done. If not, and
     * Joliet is available, prefer Joliet over plain ISO for its
     * Unicode names. User can force a specific backend.
     */
    const odfs_backend_ops_t *chosen = NULL;
    const odfs_backend_ops_t *iso_candidate = NULL;
    const odfs_backend_ops_t *joliet_candidate = NULL;
    const odfs_backend_ops_t *udf_candidate = NULL;
    const odfs_backend_ops_t *hfs_candidate = NULL;

    for (int i = 0; backend_table[i] != NULL; i++) {
        const odfs_backend_ops_t *be = backend_table[i];

        if (mnt->opts.force_backend != 0 &&
            (odfs_backend_type_t)mnt->opts.force_backend != be->backend_type)
            continue;

        ODFS_DEBUG(&mnt->log, ODFS_SUB_MOUNT,
                    "probing backend: %s", be->name);

        err = be->probe(&mnt->cache, &mnt->log, session_start);
        if (err == ODFS_OK) {
            ODFS_INFO(&mnt->log, ODFS_SUB_MOUNT,
                       "detected format: %s", be->name);
            if (be->backend_type == ODFS_BACKEND_ISO9660)
                iso_candidate = be;
            else if (be->backend_type == ODFS_BACKEND_JOLIET)
                joliet_candidate = be;
            else if (be->backend_type == ODFS_BACKEND_UDF)
                udf_candidate = be;
            else if (be->backend_type == ODFS_BACKEND_HFS)
                hfs_candidate = be;
            else
                { chosen = be; break; }
        }
    }

    /* if user prefers UDF or HFS, try those first */
    if (!chosen && udf_candidate && mnt->opts.prefer_udf) {
        err = udf_candidate->mount(&mnt->cache, &mnt->log, session_start,
                                   &mnt->root, &mnt->backend_ctx);
        if (err == ODFS_OK)
            chosen = udf_candidate;
    }
    if (!chosen && hfs_candidate && mnt->opts.prefer_hfs) {
        err = hfs_candidate->mount(&mnt->cache, &mnt->log, session_start,
                                   &mnt->root, &mnt->backend_ctx);
        if (err == ODFS_OK)
            chosen = hfs_candidate;
    }

    /* select best ISO-family candidate */
    if (!chosen && iso_candidate) {
        err = iso_candidate->mount(&mnt->cache, &mnt->log, session_start,
                                   &mnt->root, &mnt->backend_ctx);
        if (err == ODFS_OK) {
            if (mnt->root.backend == ODFS_BACKEND_ROCK_RIDGE ||
                mnt->opts.disable_joliet || !joliet_candidate) {
                chosen = iso_candidate;
            } else {
                iso_candidate->unmount(mnt->backend_ctx);
                mnt->backend_ctx = NULL;
            }
        }
    }

    if (!chosen && joliet_candidate) {
        err = joliet_candidate->mount(&mnt->cache, &mnt->log, session_start,
                                      &mnt->root, &mnt->backend_ctx);
        if (err == ODFS_OK)
            chosen = joliet_candidate;
    }

    /* UDF fallback */
    if (!chosen && udf_candidate) {
        err = udf_candidate->mount(&mnt->cache, &mnt->log, session_start,
                                   &mnt->root, &mnt->backend_ctx);
        if (err == ODFS_OK)
            chosen = udf_candidate;
    }

    /* HFS fallback (or if prefer_hfs is set) */
    if (!chosen && hfs_candidate) {
        err = hfs_candidate->mount(&mnt->cache, &mnt->log, session_start,
                                   &mnt->root, &mnt->backend_ctx);
        if (err == ODFS_OK)
            chosen = hfs_candidate;
    }

    /* last resort: plain ISO */
    if (!chosen && iso_candidate && !mnt->backend_ctx) {
        err = iso_candidate->mount(&mnt->cache, &mnt->log, session_start,
                                   &mnt->root, &mnt->backend_ctx);
        if (err == ODFS_OK)
            chosen = iso_candidate;
    }

    if (!chosen) {
        ODFS_WARN(&mnt->log, ODFS_SUB_MOUNT,
                   "no recognized filesystem format found");
        odfs_cache_destroy(&mnt->cache);
        return ODFS_ERR_BAD_FORMAT;
    }

    /* if we already mounted above, skip redundant mount */
    if (!mnt->backend_ctx) {
        err = chosen->mount(&mnt->cache, &mnt->log, session_start,
                            &mnt->root, &mnt->backend_ctx);
        if (err != ODFS_OK) {
            ODFS_ERROR(&mnt->log, ODFS_SUB_MOUNT,
                        "backend %s mount failed: %s", chosen->name,
                        odfs_err_str(err));
            odfs_cache_destroy(&mnt->cache);
            return err;
        }
    }

    mnt->backend_ops = chosen;
    mnt->active_backend = mnt->root.backend;
    odfs_mount_register_backend(mnt, mnt->root.backend, chosen,
                                mnt->backend_ctx, &mnt->root);
    if (chosen->backend_type != mnt->root.backend)
        odfs_mount_register_backend(mnt, chosen->backend_type, chosen,
                                    mnt->backend_ctx, NULL);

    /* retrieve volume name and size from backend */
    if (chosen->get_volume_name)
        chosen->get_volume_name(mnt->backend_ctx,
                                mnt->volume_name, sizeof(mnt->volume_name));
    if (chosen->get_volume_size)
        mnt->total_blocks = chosen->get_volume_size(mnt->backend_ctx);

    return ODFS_OK;
}

void odfs_unmount(odfs_mount_t *mnt)
{
    int primary_seen = 0;
    int i;

    if (!mnt)
        return;

    for (i = ODFS_BACKEND_NONE + 1; i < ODFS_BACKEND__COUNT; i++) {
        const odfs_backend_ops_t *ops = mnt->backend_map[i];
        void *ctx = mnt->backend_ctx_map[i];
        int j;
        int duplicate = 0;

        if (!ops || !ops->unmount)
            continue;

        for (j = ODFS_BACKEND_NONE + 1; j < i; j++) {
            if (mnt->backend_map[j] == ops && mnt->backend_ctx_map[j] == ctx) {
                duplicate = 1;
                break;
            }
        }
        if (duplicate)
            continue;

        if (ops == mnt->backend_ops && ctx == mnt->backend_ctx)
            primary_seen = 1;
        ops->unmount(ctx);
    }

    if (!primary_seen && mnt->backend_ops && mnt->backend_ops->unmount)
        mnt->backend_ops->unmount(mnt->backend_ctx);

    odfs_cache_destroy(&mnt->cache);
    odfs_media_close(&mnt->media);

    memset(mnt, 0, sizeof(*mnt));
}

odfs_err_t odfs_readdir(odfs_mount_t *mnt,
                          const odfs_node_t *dir,
                          odfs_dir_iter_fn callback,
                          void *ctx,
                          uint32_t *resume_offset)
{
    const odfs_backend_ops_t *ops;
    void *backend_ctx;

    if (!mnt || !dir)
        return ODFS_ERR_UNSUPPORTED;

    if (dir->kind != ODFS_NODE_DIR)
        return ODFS_ERR_NOT_DIR;

    if (!mount_backend_for_type(mnt, dir->backend, &ops, &backend_ctx) ||
        !ops || !ops->readdir)
        return ODFS_ERR_UNSUPPORTED;

    return ops->readdir(backend_ctx, &mnt->cache, &mnt->log, dir,
                        callback, ctx, resume_offset);
}

odfs_err_t odfs_read(odfs_mount_t *mnt,
                       const odfs_node_t *file,
                       uint64_t offset,
                       void *buf,
                       size_t *len)
{
    const odfs_backend_ops_t *ops;
    void *backend_ctx;

    if (!mnt || !file)
        return ODFS_ERR_UNSUPPORTED;

    if (file->kind == ODFS_NODE_DIR)
        return ODFS_ERR_IS_DIR;

    if (!mount_backend_for_type(mnt, file->backend, &ops, &backend_ctx) ||
        !ops || !ops->read)
        return ODFS_ERR_UNSUPPORTED;

    return ops->read(backend_ctx, &mnt->cache, &mnt->log, file,
                     offset, buf, len);
}

odfs_err_t odfs_lookup(odfs_mount_t *mnt,
                         const odfs_node_t *dir,
                         const char *name,
                         odfs_node_t *out)
{
    const odfs_backend_ops_t *ops;
    void *backend_ctx;

    if (!mnt || !dir)
        return ODFS_ERR_UNSUPPORTED;

    if (dir->kind != ODFS_NODE_DIR)
        return ODFS_ERR_NOT_DIR;

    if (!mount_backend_for_type(mnt, dir->backend, &ops, &backend_ctx) ||
        !ops || !ops->lookup)
        return ODFS_ERR_UNSUPPORTED;

    return ops->lookup(backend_ctx, &mnt->cache, &mnt->log, dir, name, out);
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

        if (mount_virtual_root_by_name(mnt, &current, component, &current)) {
            p = slash;
            while (*p == '/')
                p++;
            continue;
        }

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
