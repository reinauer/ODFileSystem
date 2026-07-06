/*
 * cache_meta.c — parsed-directory metadata cache
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * A directory is cached as one allocation: header, compact entry array,
 * and a string pool with the namefix-final names. Repeat lookups scan
 * the compact entries, and listing continuations resume by entry index
 * (cookies tagged with ODFS_META_COOKIE_TAG). Untagged non-zero cookies
 * are backend byte offsets and are never answered here; a tagged cookie
 * that outlives its cached directory is answered by re-enumerating the
 * backend and skipping already-delivered entries, so both cookie schemes
 * stay valid across builds and evictions.
 */

#include "odfs/cache_meta.h"

#if ODFS_FEATURE_CACHE_META

#include "odfs/alloc.h"
#include "odfs/string.h"
#include <string.h>

#define META_NO_COMMENT 0xffffffffUL

/* ------------------------------------------------------------------ */
/* LRU bookkeeping                                                     */
/* ------------------------------------------------------------------ */

static void meta_unlink(odfs_meta_cache_t *mc, odfs_meta_dir_t *d)
{
    if (d->lru_prev)
        d->lru_prev->lru_next = d->lru_next;
    else
        mc->lru_head = d->lru_next;
    if (d->lru_next)
        d->lru_next->lru_prev = d->lru_prev;
    else
        mc->lru_tail = d->lru_prev;
    d->lru_prev = NULL;
    d->lru_next = NULL;
}

static void meta_push_head(odfs_meta_cache_t *mc, odfs_meta_dir_t *d)
{
    d->lru_prev = NULL;
    d->lru_next = mc->lru_head;
    if (mc->lru_head)
        mc->lru_head->lru_prev = d;
    else
        mc->lru_tail = d;
    mc->lru_head = d;
}

static void meta_evict(odfs_meta_cache_t *mc, odfs_meta_dir_t *d)
{
    meta_unlink(mc, d);
    mc->total -= d->bytes;
    mc->stats.evictions++;
    odfs_free(d);
}

void odfs_meta_cache_init(odfs_meta_cache_t *mc, uint32_t budget_bytes)
{
    memset(mc, 0, sizeof(*mc));
    mc->budget = budget_bytes;
}

void odfs_meta_cache_flush(odfs_meta_cache_t *mc)
{
    while (mc->lru_tail)
        meta_evict(mc, mc->lru_tail);
}

void odfs_meta_cache_destroy(odfs_meta_cache_t *mc)
{
    odfs_meta_cache_flush(mc);
    memset(mc, 0, sizeof(*mc));
}

/* ------------------------------------------------------------------ */
/* lookup and node reconstruction                                      */
/* ------------------------------------------------------------------ */

static int meta_dir_cacheable(const odfs_meta_cache_t *mc,
                              const odfs_node_t *dir)
{
    if (mc->budget == 0)
        return 0;
    if (dir->kind != ODFS_NODE_DIR)
        return 0;
    if (dir->backend == ODFS_BACKEND_CDDA)
        return 0;
    if (dir->extent.length == 0)
        return 0;
    return 1;
}

static odfs_meta_dir_t *meta_find(odfs_meta_cache_t *mc,
                                  const odfs_node_t *dir)
{
    odfs_meta_dir_t *d;

    for (d = mc->lru_head; d; d = d->lru_next) {
        if (d->dir_lba == dir->extent.lba &&
            d->dir_length == dir->extent.length &&
            d->dir_backend == (uint8_t)dir->backend) {
            if (d != mc->lru_head) {
                meta_unlink(mc, d);
                meta_push_head(mc, d);
            }
            return d;
        }
    }
    return NULL;
}

static void meta_entry_to_node(const odfs_meta_dir_t *d,
                               const odfs_meta_entry_t *e,
                               odfs_node_t *out)
{
    size_t len;

    out->id = e->id;
    out->parent_id = d->dir_id;
    out->backend = (odfs_backend_type_t)e->backend;
    out->kind = (odfs_node_kind_t)e->kind;
    out->size = e->size;
    out->mtime = e->mtime;
    out->ctime = e->ctime;
    out->mode = e->mode;
    out->extent.lba = e->lba;
    out->extent.length = e->length;
    out->backend_data = NULL;

    len = strlen(d->pool + e->name_off);
    if (len >= sizeof(out->name))
        len = sizeof(out->name) - 1;
    memcpy(out->name, d->pool + e->name_off, len);
    out->name[len] = '\0';

    out->amiga_as.has_protection = e->has_protection;
    if (e->has_protection)
        memcpy(out->amiga_as.protection, e->protection, 4);
    out->amiga_as.has_comment = e->has_comment;
    if (e->has_comment) {
        len = strlen(d->pool + e->comment_off);
        if (len >= sizeof(out->amiga_as.comment))
            len = sizeof(out->amiga_as.comment) - 1;
        memcpy(out->amiga_as.comment, d->pool + e->comment_off, len);
        out->amiga_as.comment[len] = '\0';
    }
}

/* ------------------------------------------------------------------ */
/* building                                                            */
/* ------------------------------------------------------------------ */

typedef struct meta_build_ctx {
    odfs_meta_entry_t *entries;
    uint32_t           count;
    uint32_t           cap;
    char              *pool;
    uint32_t           pool_len;
    uint32_t           pool_cap;
    uint32_t           budget;
    int                failed;
} meta_build_ctx_t;

static void *meta_grow(void *old, size_t old_bytes, size_t new_bytes)
{
    void *p = odfs_malloc(new_bytes);

    if (!p)
        return NULL;
    if (old) {
        memcpy(p, old, old_bytes);
        odfs_free(old);
    }
    return p;
}

static uint32_t meta_pool_add(meta_build_ctx_t *bc, const char *s)
{
    size_t len = strlen(s) + 1u;
    uint32_t off;

    if (bc->pool_len + len > bc->pool_cap) {
        uint32_t new_cap = bc->pool_cap ? bc->pool_cap * 2u : 1024u;

        while (new_cap < bc->pool_len + len)
            new_cap *= 2u;
        bc->pool = meta_grow(bc->pool, bc->pool_len, new_cap);
        if (!bc->pool)
            return META_NO_COMMENT;
        bc->pool_cap = new_cap;
    }
    off = bc->pool_len;
    memcpy(bc->pool + off, s, len);
    bc->pool_len += (uint32_t)len;
    return off;
}

static odfs_err_t meta_build_cb(const odfs_node_t *entry, void *ctx)
{
    meta_build_ctx_t *bc = ctx;
    odfs_meta_entry_t *e;
    uint32_t off;

    if (bc->count >= bc->cap) {
        uint32_t new_cap = bc->cap ? bc->cap * 2u : 32u;

        bc->entries = meta_grow(bc->entries,
                                (size_t)bc->count * sizeof(*bc->entries),
                                (size_t)new_cap * sizeof(*bc->entries));
        if (!bc->entries)
            goto fail;
        bc->cap = new_cap;
    }

    e = &bc->entries[bc->count];
    e->id = entry->id;
    e->lba = entry->extent.lba;
    e->length = entry->extent.length;
    e->size = entry->size;
    e->mtime = entry->mtime;
    e->ctime = entry->ctime;
    e->mode = entry->mode;
    e->kind = (uint8_t)entry->kind;
    e->backend = (uint8_t)entry->backend;
    e->has_protection = entry->amiga_as.has_protection;
    if (entry->amiga_as.has_protection)
        memcpy(e->protection, entry->amiga_as.protection, 4);

    off = meta_pool_add(bc, entry->name);
    if (off == META_NO_COMMENT)
        goto fail;
    e->name_off = off;

    e->has_comment = entry->amiga_as.has_comment;
    e->comment_off = META_NO_COMMENT;
    if (entry->amiga_as.has_comment) {
        off = meta_pool_add(bc, entry->amiga_as.comment);
        if (off == META_NO_COMMENT)
            goto fail;
        e->comment_off = off;
    }

    bc->count++;

    if (sizeof(odfs_meta_dir_t) +
        (size_t)bc->count * sizeof(*bc->entries) + bc->pool_len > bc->budget)
        goto fail;

    return ODFS_OK;

fail:
    bc->failed = 1;
    return ODFS_ERR_NOMEM;
}

/*
 * Enumerate the directory through the backend and pack the result into
 * a single allocation. Returns NULL if the directory cannot be cached.
 */
static odfs_meta_dir_t *meta_build(odfs_meta_cache_t *mc,
                                   const odfs_backend_ops_t *ops,
                                   void *backend_ctx,
                                   odfs_cache_t *block_cache,
                                   odfs_log_state_t *log,
                                   const odfs_node_t *dir)
{
    meta_build_ctx_t bc;
    odfs_meta_dir_t *d = NULL;
    odfs_err_t err;
    size_t entry_bytes;
    size_t total;

    memset(&bc, 0, sizeof(bc));
    bc.budget = mc->budget;

    err = ops->readdir(backend_ctx, block_cache, log, dir,
                       meta_build_cb, &bc, NULL);
    if (bc.failed || err != ODFS_OK)
        goto out;

    entry_bytes = (size_t)bc.count * sizeof(odfs_meta_entry_t);
    total = sizeof(*d) + entry_bytes + bc.pool_len;
    d = odfs_malloc(total);
    if (!d)
        goto out;

    memset(d, 0, sizeof(*d));
    d->dir_lba = dir->extent.lba;
    d->dir_length = dir->extent.length;
    d->dir_backend = (uint8_t)dir->backend;
    d->dir_id = dir->id;
    d->count = bc.count;
    d->bytes = (uint32_t)total;
    d->entries = (odfs_meta_entry_t *)(d + 1);
    d->pool = (char *)d->entries + entry_bytes;
    memcpy(d->entries, bc.entries, entry_bytes);
    memcpy(d->pool, bc.pool, bc.pool_len);

out:
    odfs_free(bc.entries);
    odfs_free(bc.pool);
    if (d)
        mc->stats.builds++;
    else
        mc->stats.build_fails++;
    return d;
}

/*
 * Find or build the cached form of a directory. *orphan_out is set when
 * the built directory could not be kept within budget; the caller must
 * free it after use.
 */
static odfs_meta_dir_t *meta_get(odfs_meta_cache_t *mc,
                                 const odfs_backend_ops_t *ops,
                                 void *backend_ctx,
                                 odfs_cache_t *block_cache,
                                 odfs_log_state_t *log,
                                 const odfs_node_t *dir,
                                 int *orphan_out)
{
    odfs_meta_dir_t *d;

    *orphan_out = 0;

    d = meta_find(mc, dir);
    if (d) {
        mc->stats.hits++;
        return d;
    }
    mc->stats.misses++;

    d = meta_build(mc, ops, backend_ctx, block_cache, log, dir);
    if (!d)
        return NULL;

    if (d->bytes > mc->budget) {
        *orphan_out = 1;
        return d;
    }

    meta_push_head(mc, d);
    mc->total += d->bytes;
    while (mc->total > mc->budget && mc->lru_tail != d)
        meta_evict(mc, mc->lru_tail);
    return d;
}

/* ------------------------------------------------------------------ */
/* serving                                                             */
/* ------------------------------------------------------------------ */

static odfs_err_t meta_replay(const odfs_meta_dir_t *d,
                              odfs_dir_iter_fn callback,
                              void *cb_ctx,
                              uint32_t *resume_offset)
{
    uint32_t i = 0;
    odfs_err_t err;

    if (resume_offset && *resume_offset)
        i = *resume_offset & ~ODFS_META_COOKIE_TAG;

    for (; i < d->count; i++) {
        odfs_node_t node;

        meta_entry_to_node(d, &d->entries[i], &node);
        err = callback(&node, cb_ctx);
        if (err != ODFS_OK) {
            if (resume_offset)
                *resume_offset = ODFS_META_COOKIE_TAG | (i + 1u);
            return err;
        }
    }

    if (resume_offset)
        *resume_offset = ODFS_META_COOKIE_TAG | d->count;
    return ODFS_OK;
}

/* deliver entries after skipping the first `skip`, counting by index */
typedef struct meta_skip_ctx {
    uint32_t          skip;
    uint32_t          index;
    odfs_dir_iter_fn  callback;
    void             *cb_ctx;
    odfs_err_t        cb_err;
    int               stopped;
} meta_skip_ctx_t;

static odfs_err_t meta_skip_cb(const odfs_node_t *entry, void *ctx)
{
    meta_skip_ctx_t *sc = ctx;
    odfs_err_t err;

    if (sc->index++ < sc->skip)
        return ODFS_OK;

    err = sc->callback(entry, sc->cb_ctx);
    if (err != ODFS_OK) {
        sc->stopped = 1;
        sc->cb_err = err;
    }
    return err;
}

odfs_err_t odfs_meta_readdir(odfs_meta_cache_t *mc,
                             const odfs_backend_ops_t *ops,
                             void *backend_ctx,
                             odfs_cache_t *block_cache,
                             odfs_log_state_t *log,
                             const odfs_node_t *dir,
                             odfs_dir_iter_fn callback,
                             void *cb_ctx,
                             uint32_t *resume_offset)
{
    uint32_t resume = resume_offset ? *resume_offset : 0;
    int tagged = (resume & ODFS_META_COOKIE_TAG) != 0;
    odfs_meta_dir_t *d;
    int orphan;
    odfs_err_t err;

    /* untagged non-zero cookies are backend byte offsets */
    if (resume != 0 && !tagged)
        return ODFS_ERR_UNSUPPORTED;

    if (!meta_dir_cacheable(mc, dir)) {
        if (!tagged)
            return ODFS_ERR_UNSUPPORTED;
        d = NULL;
        orphan = 0;
    } else {
        d = meta_get(mc, ops, backend_ctx, block_cache, log, dir, &orphan);
    }

    if (d) {
        err = meta_replay(d, callback, cb_ctx, resume_offset);
        if (orphan)
            odfs_free(d);
        return err;
    }

    if (!tagged)
        return ODFS_ERR_UNSUPPORTED;

    /*
     * A tagged continuation whose directory can no longer be cached:
     * re-enumerate and skip the entries already delivered so the
     * index-based cookie stays valid.
     */
    {
        meta_skip_ctx_t sc;

        sc.skip = resume & ~ODFS_META_COOKIE_TAG;
        sc.index = 0;
        sc.callback = callback;
        sc.cb_ctx = cb_ctx;
        sc.cb_err = ODFS_OK;
        sc.stopped = 0;

        err = ops->readdir(backend_ctx, block_cache, log, dir,
                           meta_skip_cb, &sc, NULL);
        if (resume_offset)
            *resume_offset = ODFS_META_COOKIE_TAG | sc.index;
        if (sc.stopped)
            return sc.cb_err;
        return err;
    }
}

odfs_err_t odfs_meta_lookup(odfs_meta_cache_t *mc,
                            const odfs_backend_ops_t *ops,
                            void *backend_ctx,
                            odfs_cache_t *block_cache,
                            odfs_log_state_t *log,
                            const odfs_node_t *dir,
                            const char *name,
                            odfs_node_t *out)
{
    odfs_meta_dir_t *d;
    int orphan;
    uint32_t i;
    odfs_err_t err = ODFS_ERR_NOT_FOUND;

    if (!meta_dir_cacheable(mc, dir))
        return ODFS_ERR_UNSUPPORTED;

    d = meta_get(mc, ops, backend_ctx, block_cache, log, dir, &orphan);
    if (!d)
        return ODFS_ERR_UNSUPPORTED;

    for (i = 0; i < d->count; i++) {
        if (odfs_strcasecmp(d->pool + d->entries[i].name_off, name) == 0) {
            meta_entry_to_node(d, &d->entries[i], out);
            err = ODFS_OK;
            break;
        }
    }

    if (orphan)
        odfs_free(d);
    return err;
}

#endif /* ODFS_FEATURE_CACHE_META */
