/*
 * odfs/cache_meta.h — parsed-directory metadata cache
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Caches fully enumerated directories (namefix-final names, compact
 * entries) so repeated lookups, ExNext walks, and listing continuations
 * replay from memory instead of re-parsing on-disc records. Keyed by the
 * directory's on-disc identity, bounded by bytes, whole directories are
 * evicted LRU.
 *
 * Resume cookies issued by the cache carry ODFS_META_COOKIE_TAG and
 * encode an entry index. Untagged non-zero cookies are backend byte
 * offsets and are answered by the backend, so the two schemes can
 * coexist across cache builds and evictions.
 */

#ifndef ODFS_CACHE_META_H
#define ODFS_CACHE_META_H

#include "odfs/config.h"
#include "odfs/error.h"
#include "odfs/node.h"
#include "odfs/backend.h"

#include <stdint.h>
#include <stddef.h>

#if ODFS_FEATURE_CACHE_META

#define ODFS_META_COOKIE_TAG 0x80000000UL

/* compact cached form of one directory entry */
typedef struct odfs_meta_entry {
    uint32_t id;
    uint32_t lba;
    uint32_t length;
    uint64_t size;
    odfs_timestamp_t mtime;
    odfs_timestamp_t ctime;
    uint32_t mode;
    uint32_t name_off;     /* offset into the directory's string pool */
    uint32_t comment_off;  /* offset into pool, UINT32_MAX = no comment */
    uint8_t  kind;
    uint8_t  backend;
    uint8_t  has_protection;
    uint8_t  has_comment;
    uint8_t  protection[4];
} odfs_meta_entry_t;

/* one cached directory: a single allocation holding entries + strings */
typedef struct odfs_meta_dir {
    struct odfs_meta_dir *lru_prev;
    struct odfs_meta_dir *lru_next;
    uint32_t              dir_lba;     /* key: extent identity + backend */
    uint32_t              dir_length;
    uint8_t               dir_backend;
    uint32_t              dir_id;      /* dir node id at build time */
    uint32_t              count;
    uint32_t              bytes;       /* total allocation size */
    odfs_meta_entry_t    *entries;     /* into the same allocation */
    char                 *pool;        /* into the same allocation */
} odfs_meta_dir_t;

typedef struct odfs_meta_cache_stats {
    uint32_t hits;         /* readdir/lookup served from cache */
    uint32_t misses;       /* served by backend */
    uint32_t builds;       /* directories cached */
    uint32_t build_fails;  /* build abandoned (size/memory/error) */
    uint32_t evictions;    /* directories evicted */
} odfs_meta_cache_stats_t;

typedef struct odfs_meta_cache {
    odfs_meta_dir_t *lru_head;    /* most recently used */
    odfs_meta_dir_t *lru_tail;
    uint32_t          budget;     /* bytes; 0 disables the cache */
    uint32_t          total;      /* bytes currently held */
    odfs_meta_cache_stats_t stats;
} odfs_meta_cache_t;

void odfs_meta_cache_init(odfs_meta_cache_t *mc, uint32_t budget_bytes);
void odfs_meta_cache_destroy(odfs_meta_cache_t *mc);
void odfs_meta_cache_flush(odfs_meta_cache_t *mc);

/*
 * Serve a readdir through the cache, building the directory on first
 * use. Returns ODFS_ERR_UNSUPPORTED when the request must go to the
 * backend instead (untagged continuation cookie, uncacheable or
 * oversized directory, build failure).
 */
odfs_err_t odfs_meta_readdir(odfs_meta_cache_t *mc,
                             const odfs_backend_ops_t *ops,
                             void *backend_ctx,
                             odfs_cache_t *block_cache,
                             odfs_log_state_t *log,
                             const odfs_node_t *dir,
                             odfs_dir_iter_fn callback,
                             void *cb_ctx,
                             uint32_t *resume_offset);

/*
 * Look up one name in a directory through the cache, building it on
 * first use. Returns ODFS_ERR_UNSUPPORTED when the backend must answer.
 */
odfs_err_t odfs_meta_lookup(odfs_meta_cache_t *mc,
                            const odfs_backend_ops_t *ops,
                            void *backend_ctx,
                            odfs_cache_t *block_cache,
                            odfs_log_state_t *log,
                            const odfs_node_t *dir,
                            const char *name,
                            odfs_node_t *out);

#endif /* ODFS_FEATURE_CACHE_META */

#endif /* ODFS_CACHE_META_H */
