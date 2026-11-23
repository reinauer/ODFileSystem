/*
 * test_cache_meta.c — parsed-directory cache bookkeeping
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "odfs/api.h"
#include "odfs/cache_meta.h"
#include "odfs/log.h"
#include "test_harness.h"

#include <stdio.h>
#include <string.h>

/* every directory holds one file, "x", whose lba follows the dir's */
static odfs_err_t one_file_readdir(void *backend_ctx, odfs_cache_t *cache,
                                   odfs_log_state_t *log,
                                   const odfs_node_t *dir,
                                   odfs_dir_iter_fn callback, void *cb_ctx,
                                   uint32_t *resume_offset)
{
    odfs_node_t child;
    int *readdirs = backend_ctx;

    (void)cache;
    (void)log;
    (*readdirs)++;

    memset(&child, 0, sizeof(child));
    child.id = dir->extent.lba + 1;
    child.parent_id = dir->id;
    child.backend = dir->backend;
    child.kind = ODFS_NODE_FILE;
    child.extent.lba = dir->extent.lba + 1;
    child.extent.length = 1;
    child.size = 1;
    strcpy(child.name, "x");
    if (resume_offset)
        *resume_offset = 1;
    return callback(&child, cb_ctx);
}

static const odfs_backend_ops_t one_file_ops = {
    .name = "one-file",
    .backend_type = ODFS_BACKEND_ISO9660,
    .readdir = one_file_readdir,
};

static void init_dir(odfs_node_t *dir, uint32_t lba)
{
    memset(dir, 0, sizeof(*dir));
    dir->id = lba;
    dir->backend = ODFS_BACKEND_ISO9660;
    dir->kind = ODFS_NODE_DIR;
    dir->extent.lba = lba;
    dir->extent.length = 2048;
    dir->size = 2048;
    snprintf(dir->name, sizeof(dir->name), "d%u", (unsigned)lba);
}

#define LOOKUP_X(mc, log, readdirs, dir, out) \
    ASSERT_OK(odfs_meta_lookup((mc), &one_file_ops, (readdirs), NULL, \
                               (log), (dir), "x", (out)))

TEST(meta_cache_evicts_least_recently_used_directory)
{
    static odfs_meta_cache_t mc;
    odfs_log_state_t log;
    odfs_node_t a, b, c, out;
    int readdirs = 0;
    uint32_t one;

    odfs_log_init(&log);
    init_dir(&a, 100);
    init_dir(&b, 200);
    init_dir(&c, 300);

    /* measure one cached directory, then allow room for two and a half */
    odfs_meta_cache_init(&mc, 1u << 20);
    LOOKUP_X(&mc, &log, &readdirs, &a, &out);
    one = mc.total;
    odfs_meta_cache_destroy(&mc);
    odfs_meta_cache_init(&mc, one * 5u / 2u);
    readdirs = 0;

    LOOKUP_X(&mc, &log, &readdirs, &a, &out);
    LOOKUP_X(&mc, &log, &readdirs, &b, &out);
    /* a is near the head and is found without moving it there */
    LOOKUP_X(&mc, &log, &readdirs, &a, &out);
    ASSERT_EQ(readdirs, 2);

    /* building c must evict b, used less recently than a */
    LOOKUP_X(&mc, &log, &readdirs, &c, &out);
    ASSERT_EQ(readdirs, 3);
    ASSERT_EQ(mc.stats.evictions, 1);
    ASSERT(mc.total <= mc.budget);

    LOOKUP_X(&mc, &log, &readdirs, &a, &out);
    ASSERT_EQ(readdirs, 3);
    LOOKUP_X(&mc, &log, &readdirs, &b, &out);
    ASSERT_EQ(readdirs, 4);

    odfs_meta_cache_destroy(&mc);
}

/*
 * HFS and HFS+ directories: a backend wrapper records the nodes the
 * backend delivers while the cache builds a directory, and the cached
 * replay and lookups must hand back exactly those nodes.
 */
#define REC_MAX 32

typedef struct node_list {
    odfs_node_t  n[REC_MAX];
    unsigned int count;
} node_list_t;

typedef struct rec_backend {
    const odfs_backend_ops_t *ops;
    void                     *ctx;
    node_list_t               seen;
    int                       readdirs;
} rec_backend_t;

typedef struct rec_fwd {
    odfs_dir_iter_fn  callback;
    void             *cb_ctx;
    node_list_t      *seen;
} rec_fwd_t;

static odfs_err_t node_list_add(const odfs_node_t *entry, void *ctx)
{
    node_list_t *l = ctx;

    if (l->count >= REC_MAX)
        return ODFS_ERR_NOMEM;
    l->n[l->count++] = *entry;
    return ODFS_OK;
}

static odfs_err_t rec_fwd_cb(const odfs_node_t *entry, void *ctx)
{
    rec_fwd_t *f = ctx;

    node_list_add(entry, f->seen);
    return f->callback(entry, f->cb_ctx);
}

static odfs_err_t rec_readdir(void *backend_ctx, odfs_cache_t *cache,
                              odfs_log_state_t *log,
                              const odfs_node_t *dir,
                              odfs_dir_iter_fn callback, void *cb_ctx,
                              uint32_t *resume_offset)
{
    rec_backend_t *rb = backend_ctx;
    rec_fwd_t f;

    f.callback = callback;
    f.cb_ctx = cb_ctx;
    f.seen = &rb->seen;
    rb->seen.count = 0;
    rb->readdirs++;
    return rb->ops->readdir(rb->ctx, cache, log, dir, rec_fwd_cb, &f,
                            resume_offset);
}

#define ASSERT_SAME_ENTRY(a, b) \
    do { \
        ASSERT_EQ((a)->id, (b)->id); \
        ASSERT_EQ((a)->parent_id, (b)->parent_id); \
        ASSERT_EQ((a)->backend, (b)->backend); \
        ASSERT_EQ((a)->kind, (b)->kind); \
        ASSERT_EQ((a)->extent.lba, (b)->extent.lba); \
        ASSERT_EQ((a)->extent.length, (b)->extent.length); \
        ASSERT_EQ((a)->size, (b)->size); \
        ASSERT_EQ((a)->mtime.year, (b)->mtime.year); \
        ASSERT_EQ((a)->mtime.second, (b)->mtime.second); \
        ASSERT_STR_EQ((a)->name, (b)->name); \
    } while (0)

/* list dir through the cache and check it against the backend's nodes */
static void check_cached_listing(int *_fail_count, odfs_meta_cache_t *mc,
                                 const odfs_backend_ops_t *ops,
                                 rec_backend_t *rb, odfs_mount_t *mnt,
                                 const odfs_node_t *dir)
{
    static node_list_t replay;
    int readdirs = rb->readdirs;
    unsigned int i;

    replay.count = 0;
    ASSERT_OK(odfs_meta_readdir(mc, ops, rb, &mnt->cache, &mnt->log, dir,
                                node_list_add, &replay, NULL));
    ASSERT_EQ(rb->readdirs, readdirs + 1);
    ASSERT(rb->seen.count > 0);
    ASSERT_EQ(replay.count, rb->seen.count);
    for (i = 0; i < replay.count; i++)
        ASSERT_SAME_ENTRY(&replay.n[i], &rb->seen.n[i]);

    /* the second listing comes from the cache alone */
    replay.count = 0;
    ASSERT_OK(odfs_meta_readdir(mc, ops, rb, &mnt->cache, &mnt->log, dir,
                                node_list_add, &replay, NULL));
    ASSERT_EQ(rb->readdirs, readdirs + 1);
    ASSERT_EQ(replay.count, rb->seen.count);
    for (i = 0; i < replay.count; i++)
        ASSERT_SAME_ENTRY(&replay.n[i], &rb->seen.n[i]);
}

static void check_hfs_cache(int *_fail_count, const char *image,
                            odfs_backend_type_t backend,
                            const char *deep_name, const char *path)
{
    static odfs_meta_cache_t mc;
    static odfs_mount_t mnt;
    static rec_backend_t rb;
    static odfs_backend_ops_t ops;
    static odfs_node_t deep, subdir, out, parent, grandparent;
    odfs_media_t media;
    odfs_log_state_t log;
    odfs_mount_opts_t opts;
    unsigned int i, depth;

    if (odfs_media_open_image(image, &media) != ODFS_OK) {
        printf("  (skipped: fixture %s unavailable)\n", image);
        return;
    }
    odfs_log_init(&log);
    odfs_mount_opts_default(&opts);
    opts.force_backend = backend;
    opts.meta_cache_kib = 0;
    if (odfs_mount(&media, &opts, &log, &mnt) != ODFS_OK) {
        odfs_media_close(&media);
        printf("  (skipped: fixture %s does not mount)\n", image);
        return;
    }
    ASSERT_EQ(mnt.root.backend, backend);
    ASSERT_EQ(mnt.root.extent.length, 0);

    memset(&rb, 0, sizeof(rb));
    rb.ops = mnt.backend_ops;
    rb.ctx = mnt.backend_ctx;
    ops = *mnt.backend_ops;
    ops.readdir = rec_readdir;
    odfs_meta_cache_init(&mc, 1u << 16);

    check_cached_listing(_fail_count, &mc, &ops, &rb, &mnt, &mnt.root);

    /* a lookup in the cached root returns the node the backend gave */
    ASSERT_OK(odfs_meta_lookup(&mc, &ops, &rb, &mnt.cache, &mnt.log,
                               &mnt.root, deep_name, &deep));
    ASSERT_EQ(rb.readdirs, 1);
    ASSERT_EQ(deep.kind, ODFS_NODE_DIR);
    for (i = 0; i < rb.seen.count; i++) {
        if (rb.seen.n[i].id == deep.id)
            break;
    }
    ASSERT(i < rb.seen.count);
    ASSERT_SAME_ENTRY(&deep, &rb.seen.n[i]);

    check_cached_listing(_fail_count, &mc, &ops, &rb, &mnt, &deep);
    for (i = 0; i < rb.seen.count; i++) {
        ASSERT_EQ(rb.seen.n[i].parent_id, deep.id);
        if (rb.seen.n[i].kind == ODFS_NODE_DIR)
            subdir = rb.seen.n[i];
    }
    ASSERT_EQ(subdir.kind, ODFS_NODE_DIR);

    /* a path walk through both cached directories agrees too */
    ASSERT_OK(odfs_meta_lookup_path(&mc, &mnt.root, path, &out, &parent,
                                    &grandparent, &depth));
    ASSERT_EQ(depth, 2);
    ASSERT_SAME_ENTRY(&out, &subdir);
    ASSERT_SAME_ENTRY(&parent, &deep);
    ASSERT_EQ(rb.readdirs, 2);

    odfs_meta_cache_destroy(&mc);
    odfs_unmount(&mnt);
}

TEST(meta_cache_serves_hfs_directories)
{
    check_hfs_cache(_fail_count, "tests/images/test_hfs.iso",
                    ODFS_BACKEND_HFS, "deep", "DEEP/SUBDIR");
}

TEST(meta_cache_serves_hfsplus_directories)
{
    check_hfs_cache(_fail_count, "tests/images/test_hfsplus.iso",
                    ODFS_BACKEND_HFSPLUS, "deep", "deep/subdir");
}

TEST_MAIN()
