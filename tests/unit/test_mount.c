/*
 * test_mount.c — tests for generic mount dispatch helpers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "odfs/api.h"
#include "odfs/error.h"
#include "test_harness.h"

#include <stdint.h>
#include <string.h>

typedef struct fake_backend_ctx {
    int lookup_calls;
    int readdir_calls;
    int read_calls;
} fake_backend_ctx_t;

static const uint8_t fake_track_data[] = { 'R', 'I', 'F', 'F' };

static odfs_err_t fake_main_probe(odfs_cache_t *cache,
                                  odfs_log_state_t *log,
                                  uint32_t session_start)
{
    (void)cache;
    (void)log;
    (void)session_start;
    return ODFS_OK;
}

static odfs_err_t fake_main_mount(odfs_cache_t *cache,
                                  odfs_log_state_t *log,
                                  uint32_t session_start,
                                  odfs_node_t *root_out,
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

static odfs_err_t fake_main_readdir(void *backend_ctx,
                                    odfs_cache_t *cache,
                                    odfs_log_state_t *log,
                                    const odfs_node_t *dir,
                                    odfs_dir_iter_fn callback,
                                    void *cb_ctx,
                                    uint32_t *resume_offset)
{
    (void)backend_ctx;
    (void)cache;
    (void)log;
    (void)dir;
    (void)callback;
    (void)cb_ctx;
    (void)resume_offset;
    return ODFS_OK;
}

static odfs_err_t fake_main_read(void *backend_ctx,
                                 odfs_cache_t *cache,
                                 odfs_log_state_t *log,
                                 const odfs_node_t *file,
                                 uint64_t offset,
                                 void *buf,
                                 size_t *len)
{
    (void)backend_ctx;
    (void)cache;
    (void)log;
    (void)file;
    (void)offset;
    (void)buf;
    (void)len;
    return ODFS_ERR_NOT_FOUND;
}

static odfs_err_t fake_main_lookup(void *backend_ctx,
                                   odfs_cache_t *cache,
                                   odfs_log_state_t *log,
                                   const odfs_node_t *dir,
                                   const char *name,
                                   odfs_node_t *out)
{
    fake_backend_ctx_t *ctx = backend_ctx;
    (void)cache;
    (void)log;
    (void)dir;
    (void)name;
    (void)out;
    ctx->lookup_calls++;
    return ODFS_ERR_NOT_FOUND;
}

static odfs_err_t fake_get_volume_name(void *backend_ctx,
                                       char *buf,
                                       size_t buf_size)
{
    (void)backend_ctx;
    if (buf_size < 5)
        return ODFS_ERR_RANGE;
    memcpy(buf, "fake", 5);
    return ODFS_OK;
}

static uint32_t fake_get_volume_size(void *backend_ctx)
{
    (void)backend_ctx;
    return 1;
}

static const odfs_backend_ops_t fake_main_backend_ops = {
    .name = "fake-main",
    .backend_type = ODFS_BACKEND_ISO9660,
    .probe = fake_main_probe,
    .mount = fake_main_mount,
    .unmount = fake_unmount,
    .readdir = fake_main_readdir,
    .read = fake_main_read,
    .lookup = fake_main_lookup,
    .get_volume_name = fake_get_volume_name,
    .get_volume_size = fake_get_volume_size,
};

static odfs_err_t fake_cdda_readdir(void *backend_ctx,
                                    odfs_cache_t *cache,
                                    odfs_log_state_t *log,
                                    const odfs_node_t *dir,
                                    odfs_dir_iter_fn callback,
                                    void *cb_ctx,
                                    uint32_t *resume_offset)
{
    fake_backend_ctx_t *ctx = backend_ctx;
    odfs_node_t node;

    (void)cache;
    (void)log;
    (void)dir;
    (void)resume_offset;

    ctx->readdir_calls++;
    memset(&node, 0, sizeof(node));
    node.id = 1;
    node.backend = ODFS_BACKEND_CDDA;
    node.kind = ODFS_NODE_VIRTUAL;
    node.size = sizeof(fake_track_data);
    node.extent.lba = 2;
    node.extent.length = sizeof(fake_track_data);
    memcpy(node.name, "Track01.wav", 12);

    return callback(&node, cb_ctx);
}

static odfs_err_t fake_cdda_read(void *backend_ctx,
                                 odfs_cache_t *cache,
                                 odfs_log_state_t *log,
                                 const odfs_node_t *file,
                                 uint64_t offset,
                                 void *buf,
                                 size_t *len)
{
    fake_backend_ctx_t *ctx = backend_ctx;
    size_t want;

    (void)cache;
    (void)log;
    (void)file;

    ctx->read_calls++;
    if (offset >= sizeof(fake_track_data)) {
        *len = 0;
        return ODFS_OK;
    }

    want = *len;
    if (offset + want > sizeof(fake_track_data))
        want = sizeof(fake_track_data) - (size_t)offset;
    memcpy(buf, fake_track_data + offset, want);
    *len = want;
    return ODFS_OK;
}

static odfs_err_t fake_cdda_lookup(void *backend_ctx,
                                   odfs_cache_t *cache,
                                   odfs_log_state_t *log,
                                   const odfs_node_t *dir,
                                   const char *name,
                                   odfs_node_t *out)
{
    fake_backend_ctx_t *ctx = backend_ctx;

    (void)cache;
    (void)log;
    (void)dir;

    ctx->lookup_calls++;
    if (strcmp(name, "Track01.wav") != 0)
        return ODFS_ERR_NOT_FOUND;

    memset(out, 0, sizeof(*out));
    out->id = 1;
    out->backend = ODFS_BACKEND_CDDA;
    out->kind = ODFS_NODE_VIRTUAL;
    out->size = sizeof(fake_track_data);
    out->extent.lba = 2;
    out->extent.length = sizeof(fake_track_data);
    memcpy(out->name, "Track01.wav", 12);
    return ODFS_OK;
}

static const odfs_backend_ops_t fake_cdda_backend_ops = {
    .name = "fake-cdda",
    .backend_type = ODFS_BACKEND_CDDA,
    .probe = fake_main_probe,
    .mount = fake_main_mount,
    .unmount = fake_unmount,
    .readdir = fake_cdda_readdir,
    .read = fake_cdda_read,
    .lookup = fake_cdda_lookup,
    .get_volume_name = fake_get_volume_name,
    .get_volume_size = fake_get_volume_size,
};

typedef struct collect_ctx {
    int count;
    odfs_node_t first;
} collect_ctx_t;

static odfs_err_t collect_first_entry(const odfs_node_t *entry, void *ctx)
{
    collect_ctx_t *collect = ctx;

    collect->count++;
    if (collect->count == 1)
        collect->first = *entry;
    return ODFS_OK;
}

TEST(mount_dispatches_virtual_backend_by_node_type)
{
    odfs_mount_t mnt;
    fake_backend_ctx_t main_ctx;
    fake_backend_ctx_t cdda_ctx;
    odfs_node_t root;
    odfs_node_t cdda_root;
    odfs_node_t file;
    odfs_node_t dir;
    collect_ctx_t collect;
    uint8_t buf[8];
    size_t len;

    memset(&mnt, 0, sizeof(mnt));
    memset(&main_ctx, 0, sizeof(main_ctx));
    memset(&cdda_ctx, 0, sizeof(cdda_ctx));
    memset(&root, 0, sizeof(root));
    memset(&cdda_root, 0, sizeof(cdda_root));

    root.backend = ODFS_BACKEND_ISO9660;
    root.kind = ODFS_NODE_DIR;
    root.id = 100;
    root.extent.lba = 42;
    root.extent.length = 2048;
    root.size = 2048;
    memcpy(root.name, "/", 2);

    cdda_root.backend = ODFS_BACKEND_CDDA;
    cdda_root.kind = ODFS_NODE_DIR;
    cdda_root.id = 0;
    memcpy(cdda_root.name, "CDDA", 5);

    mnt.root = root;
    mnt.backend_ops = &fake_main_backend_ops;
    mnt.backend_ctx = &main_ctx;
    mnt.active_backend = root.backend;

    odfs_mount_register_backend(&mnt, ODFS_BACKEND_ISO9660,
                                &fake_main_backend_ops, &main_ctx, &root);
    odfs_mount_register_backend(&mnt, ODFS_BACKEND_CDDA,
                                &fake_cdda_backend_ops, &cdda_ctx, &cdda_root);

    ASSERT_OK(odfs_resolve_path(&mnt, "CDDA", &dir));
    ASSERT_EQ(dir.backend, ODFS_BACKEND_CDDA);
    ASSERT_STR_EQ(dir.name, "CDDA");

    ASSERT_OK(odfs_resolve_path(&mnt, "CDDA/Track01.wav", &file));
    ASSERT_EQ(file.backend, ODFS_BACKEND_CDDA);
    ASSERT_STR_EQ(file.name, "Track01.wav");
    ASSERT_EQ(main_ctx.lookup_calls, 0);
    ASSERT_EQ(cdda_ctx.lookup_calls, 1);

    memset(&collect, 0, sizeof(collect));
    ASSERT_OK(odfs_readdir(&mnt, &dir, collect_first_entry, &collect, NULL));
    ASSERT_EQ(collect.count, 1);
    ASSERT_STR_EQ(collect.first.name, "Track01.wav");
    ASSERT_EQ(cdda_ctx.readdir_calls, 1);

    memset(buf, 0, sizeof(buf));
    len = sizeof(buf);
    ASSERT_OK(odfs_read(&mnt, &file, 0, buf, &len));
    ASSERT_EQ(len, sizeof(fake_track_data));
    ASSERT(memcmp(buf, fake_track_data, sizeof(fake_track_data)) == 0);
    ASSERT_EQ(cdda_ctx.read_calls, 1);
}

TEST_MAIN()
