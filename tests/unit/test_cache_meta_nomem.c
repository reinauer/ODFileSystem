/*
 * test_cache_meta_nomem.c — parsed-directory cache when memory runs out
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Builds the cache with an allocator that can be told to fail from the
 * Nth allocation on, and that counts the blocks it has handed out.
 */

#include "odfs/alloc.h"

#include <stdlib.h>

static int allocs_left = -1;    /* allocations that still succeed; -1: all */
static int blocks_out;          /* handed out and not freed */

static void *counting_malloc(size_t size)
{
    void *p;

    if (allocs_left == 0)
        return NULL;
    if (allocs_left > 0)
        allocs_left--;
    p = malloc(size);
    if (p)
        blocks_out++;
    return p;
}

static void counting_free(void *p)
{
    if (p) {
        blocks_out--;
        free(p);
    }
}

#define odfs_malloc(size) counting_malloc(size)
#define odfs_free(ptr)    counting_free(ptr)
#include "../../core/cache_meta.c"

#include "odfs/log.h"
#include "test_harness.h"

#include <stdio.h>

/*
 * More files, with longer names, than the build buffers are sized for
 * from a directory this short: both have to grow several times.
 */
#define MANY_FILES 100u

static odfs_err_t many_files_readdir(void *backend_ctx, odfs_cache_t *cache,
                                     odfs_log_state_t *log,
                                     const odfs_node_t *dir,
                                     odfs_dir_iter_fn callback, void *cb_ctx,
                                     uint32_t *resume_offset)
{
    odfs_node_t child;
    uint32_t i;

    (void)backend_ctx;
    (void)cache;
    (void)log;

    for (i = 0; i < MANY_FILES; i++) {
        odfs_err_t err;

        memset(&child, 0, sizeof(child));
        child.id = dir->extent.lba + 1u + i;
        child.parent_id = dir->id;
        child.backend = dir->backend;
        child.kind = ODFS_NODE_FILE;
        child.extent.lba = dir->extent.lba + 1u + i;
        child.extent.length = 1;
        child.size = 1;
        snprintf(child.name, sizeof(child.name),
                 "a file with a long name %03u", (unsigned)i);
        if (resume_offset)
            *resume_offset = i + 1u;
        err = callback(&child, cb_ctx);
        if (err != ODFS_OK)
            return err;
    }
    return ODFS_OK;
}

static const odfs_backend_ops_t many_files_ops = {
    .name = "many-files",
    .backend_type = ODFS_BACKEND_ISO9660,
    .readdir = many_files_readdir,
};

TEST(meta_cache_build_leaks_nothing_when_memory_runs_out)
{
    static odfs_meta_cache_t mc;
    odfs_log_state_t log;
    odfs_node_t dir, out;
    int allowed;
    int built = 0;

    odfs_log_init(&log);
    memset(&dir, 0, sizeof(dir));
    dir.id = 100;
    dir.backend = ODFS_BACKEND_ISO9660;
    dir.kind = ODFS_NODE_DIR;
    dir.extent.lba = 100;
    dir.extent.length = 34;
    dir.size = 34;
    strcpy(dir.name, "d");

    /* let one more allocation succeed each round, until the build does */
    for (allowed = 0; allowed < 64 && !built; allowed++) {
        odfs_meta_cache_init(&mc, 1u << 20);
        allocs_left = allowed;
        (void)odfs_meta_lookup(&mc, &many_files_ops, NULL, NULL, &log, &dir,
                               "a file with a long name 042", &out);
        allocs_left = -1;
        built = mc.stats.builds != 0;
        if (!built)
            ASSERT_EQ(blocks_out, 0);
        odfs_meta_cache_destroy(&mc);
        ASSERT_EQ(blocks_out, 0);
    }

    ASSERT(built);
    /* two buffers and the cached directory, plus the growing */
    ASSERT(allowed > 6);
}

TEST_MAIN()
