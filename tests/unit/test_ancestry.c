/*
 * test_ancestry.c — tests for parent/ancestor lookup helpers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "odfs/ancestry.h"
#include "odfs/api.h"
#include "odfs/backend.h"
#include "test_harness.h"

#include <string.h>

typedef struct fake_tree_ctx {
    odfs_node_t root;
    odfs_node_t a;
    odfs_node_t b;
    odfs_node_t c;
    odfs_node_t d;
    odfs_node_t x;
} fake_tree_ctx_t;

static void fake_init_node(odfs_node_t *node, uint32_t id, const char *name,
                           odfs_node_kind_t kind, uint32_t lba)
{
    memset(node, 0, sizeof(*node));
    node->id = id;
    node->backend = ODFS_BACKEND_ISO9660;
    node->kind = kind;
    node->extent.lba = lba;
    node->extent.length = 2048;
    node->size = 2048;
    strcpy(node->name, name);
}

static void fake_init_tree(fake_tree_ctx_t *ctx)
{
    fake_init_node(&ctx->root, 1, "/", ODFS_NODE_DIR, 10);
    fake_init_node(&ctx->a, 2, "a", ODFS_NODE_DIR, 11);
    fake_init_node(&ctx->b, 3, "b", ODFS_NODE_DIR, 12);
    fake_init_node(&ctx->c, 4, "c", ODFS_NODE_DIR, 13);
    fake_init_node(&ctx->d, 5, "d", ODFS_NODE_DIR, 14);
    fake_init_node(&ctx->x, 6, "x", ODFS_NODE_DIR, 15);

    ctx->a.parent_id = ctx->root.id;
    ctx->b.parent_id = ctx->a.id;
    ctx->c.parent_id = ctx->b.id;
    ctx->d.parent_id = ctx->c.id;
    ctx->x.parent_id = ctx->a.id;
}

static size_t fake_children(fake_tree_ctx_t *ctx, const odfs_node_t *dir,
                            const odfs_node_t **children)
{
    if (dir->extent.lba == ctx->root.extent.lba) {
        children[0] = &ctx->a;
        return 1;
    }
    if (dir->extent.lba == ctx->a.extent.lba) {
        children[0] = &ctx->b;
        children[1] = &ctx->x;
        return 2;
    }
    if (dir->extent.lba == ctx->b.extent.lba) {
        children[0] = &ctx->c;
        return 1;
    }
    if (dir->extent.lba == ctx->c.extent.lba) {
        children[0] = &ctx->d;
        return 1;
    }

    return 0;
}

static odfs_err_t fake_probe(odfs_cache_t *cache, odfs_log_state_t *log,
                             uint32_t session_start)
{
    (void)cache;
    (void)log;
    (void)session_start;
    return ODFS_ERR_UNSUPPORTED;
}

static odfs_err_t fake_mount_fn(odfs_cache_t *cache, odfs_log_state_t *log,
                                uint32_t session_start, odfs_node_t *root_out,
                                void **backend_ctx)
{
    (void)cache;
    (void)log;
    (void)session_start;
    (void)root_out;
    (void)backend_ctx;
    return ODFS_ERR_UNSUPPORTED;
}

static void fake_unmount(void *backend_ctx)
{
    (void)backend_ctx;
}

static odfs_err_t fake_readdir(void *backend_ctx, odfs_cache_t *cache,
                               odfs_log_state_t *log, const odfs_node_t *dir,
                               odfs_dir_iter_fn callback, void *cb_ctx,
                               uint32_t *resume_offset)
{
    fake_tree_ctx_t *ctx = backend_ctx;
    const odfs_node_t *children[4];
    size_t count;
    uint32_t index;

    (void)cache;
    (void)log;

    count = fake_children(ctx, dir, children);
    index = resume_offset ? *resume_offset : 0;

    while (index < count) {
        odfs_err_t err = callback(children[index], cb_ctx);

        index++;
        if (err != ODFS_OK) {
            if (resume_offset)
                *resume_offset = index;
            return err;
        }
    }

    if (resume_offset)
        *resume_offset = (uint32_t)count;
    return ODFS_OK;
}

static odfs_err_t fake_read(void *backend_ctx, odfs_cache_t *cache,
                            odfs_log_state_t *log, const odfs_node_t *file,
                            uint64_t offset, void *buf, size_t *len)
{
    (void)backend_ctx;
    (void)cache;
    (void)log;
    (void)file;
    (void)offset;
    (void)buf;
    (void)len;
    return ODFS_ERR_UNSUPPORTED;
}

static odfs_err_t fake_lookup(void *backend_ctx, odfs_cache_t *cache,
                              odfs_log_state_t *log, const odfs_node_t *dir,
                              const char *name, odfs_node_t *out)
{
    fake_tree_ctx_t *ctx = backend_ctx;
    const odfs_node_t *children[4];
    size_t count;
    size_t i;

    (void)cache;
    (void)log;

    count = fake_children(ctx, dir, children);
    for (i = 0; i < count; i++) {
        if (strcmp(children[i]->name, name) == 0) {
            *out = *children[i];
            return ODFS_OK;
        }
    }

    return ODFS_ERR_NOT_FOUND;
}

static const odfs_backend_ops_t fake_backend_ops = {
    .name = "fake-tree",
    .backend_type = ODFS_BACKEND_ISO9660,
    .probe = fake_probe,
    .mount = fake_mount_fn,
    .unmount = fake_unmount,
    .readdir = fake_readdir,
    .read = fake_read,
    .lookup = fake_lookup,
    .get_volume_name = NULL,
    .get_volume_size = NULL,
};

static void fake_init_mount(odfs_mount_t *mnt, fake_tree_ctx_t *ctx)
{
    memset(mnt, 0, sizeof(*mnt));
    memset(ctx, 0, sizeof(*ctx));

    fake_init_tree(ctx);

    mnt->root = ctx->root;
    mnt->active_backend = ODFS_BACKEND_ISO9660;
    mnt->backend_ops = &fake_backend_ops;
    mnt->backend_ctx = ctx;
    mnt->backend_map[ODFS_BACKEND_ISO9660] = &fake_backend_ops;
    mnt->backend_ctx_map[ODFS_BACKEND_ISO9660] = ctx;
}

TEST(resolve_parent_node_handles_deep_chain_without_stable_ids)
{
    fake_tree_ctx_t ctx;
    odfs_mount_t mnt;
    odfs_node_t probe;
    odfs_node_t parent;
    odfs_node_t grandparent;

    fake_init_mount(&mnt, &ctx);

    probe = ctx.d;
    probe.id = 9001;
    ASSERT_OK(odfs_resolve_parent_node(&mnt, &probe, &parent, &grandparent));
    ASSERT_STR_EQ(parent.name, "c");
    ASSERT_STR_EQ(grandparent.name, "b");

    probe = parent;
    probe.id = 9002;
    ASSERT_OK(odfs_resolve_parent_node(&mnt, &probe, &parent, &grandparent));
    ASSERT_STR_EQ(parent.name, "b");
    ASSERT_STR_EQ(grandparent.name, "a");

    probe = parent;
    probe.id = 9003;
    ASSERT_OK(odfs_resolve_parent_node(&mnt, &probe, &parent, &grandparent));
    ASSERT_STR_EQ(parent.name, "a");
    ASSERT_STR_EQ(grandparent.name, "/");

    probe = parent;
    probe.id = 9004;
    ASSERT_OK(odfs_resolve_parent_node(&mnt, &probe, &parent, &grandparent));
    ASSERT_STR_EQ(parent.name, "/");
    ASSERT_STR_EQ(grandparent.name, "/");
}

TEST(resolve_parent_node_supports_multi_ascent_then_lookup)
{
    fake_tree_ctx_t ctx;
    odfs_mount_t mnt;
    odfs_node_t cur;
    odfs_node_t parent;
    odfs_node_t grandparent;
    odfs_node_t child;

    fake_init_mount(&mnt, &ctx);

    cur = ctx.d;
    parent = ctx.c;

    cur = parent;
    ASSERT_OK(odfs_resolve_parent_node(&mnt, &cur, &parent, &grandparent));
    ASSERT_STR_EQ(cur.name, "c");
    ASSERT_STR_EQ(parent.name, "b");

    cur = parent;
    ASSERT_OK(odfs_resolve_parent_node(&mnt, &cur, &parent, &grandparent));
    ASSERT_STR_EQ(cur.name, "b");
    ASSERT_STR_EQ(parent.name, "a");

    cur = parent;
    ASSERT_OK(odfs_resolve_parent_node(&mnt, &cur, &parent, &grandparent));
    ASSERT_STR_EQ(cur.name, "a");
    ASSERT_STR_EQ(parent.name, "/");

    ASSERT_OK(odfs_lookup(&mnt, &cur, "x", &child));
    ASSERT_STR_EQ(child.name, "x");
}

TEST_MAIN()
