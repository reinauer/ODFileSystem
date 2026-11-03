/*
 * test_cache_meta.c — parsed-directory cache bookkeeping
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

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

TEST_MAIN()
