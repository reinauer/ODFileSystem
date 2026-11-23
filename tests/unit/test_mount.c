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

    /* registering the mount root keeps the CDDA directory */
    odfs_mount_register_backend(&mnt, ODFS_BACKEND_ISO9660,
                                &fake_main_backend_ops, &main_ctx, &root);
    ASSERT_OK(odfs_resolve_path(&mnt, "CDDA", &dir));
    ASSERT_EQ(dir.backend, ODFS_BACKEND_CDDA);

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

    /* a backend that does not declare contiguous files keeps its reads */
    {
        uint32_t lba = 0;

        ASSERT_EQ(odfs_file_is_contiguous(&mnt, &file, &lba), 0);
    }
}

static odfs_err_t io_fail_read_sectors(void *ctx, uint32_t lba,
                                       uint32_t count, void *buf)
{
    (void)ctx;
    (void)lba;
    (void)count;
    (void)buf;
    return ODFS_ERR_IO;
}

static uint32_t io_fail_sector_size(void *ctx)
{
    (void)ctx;
    return 2048;
}

static uint32_t io_fail_sector_count(void *ctx)
{
    (void)ctx;
    return 64;
}

static const odfs_media_ops_t io_fail_ops = {
    .read_sectors = io_fail_read_sectors,
    .sector_size  = io_fail_sector_size,
    .sector_count = io_fail_sector_count,
};

TEST(mount_reports_media_errors)
{
    odfs_media_t media = { &io_fail_ops, NULL };
    odfs_mount_t mnt;

    ASSERT_ERR(odfs_mount(&media, NULL, NULL, &mnt), ODFS_ERR_IO);
}

#define IMG_RR "tests/images/test_rr.iso"

/* test_rr.iso with an HFS+ volume header signature in its system area */
static odfs_err_t hybrid_read_sectors(void *ctx, uint32_t lba,
                                      uint32_t count, void *buf)
{
    odfs_media_t *inner = ctx;
    uint8_t *out = buf;
    odfs_err_t err = odfs_media_read(inner, lba, count, buf);

    /* probe accepts 'H+' version 4 with 4 KiB blocks; the catalog
     * fields stay zero, so mounting the HFS+ view fails */
    if (err == ODFS_OK && lba == 0) {
        out[1024] = 'H';
        out[1025] = '+';
        out[1026] = 0;
        out[1027] = 4;
        out[1024 + 42] = 0x10;
    }
    return err;
}

static uint32_t hybrid_sector_size(void *ctx)
{
    return odfs_media_sector_size(ctx);
}

static uint32_t hybrid_sector_count(void *ctx)
{
    return odfs_media_sector_count(ctx);
}

TEST(mount_prefers_iso_family_over_hfsplus_on_hybrid)
{
    static const odfs_media_ops_t hybrid_ops = {
        .read_sectors = hybrid_read_sectors,
        .sector_size  = hybrid_sector_size,
        .sector_count = hybrid_sector_count,
    };
    odfs_media_t inner;
    odfs_media_t media = { &hybrid_ops, &inner };
    odfs_mount_opts_t opts;
    odfs_mount_t mnt;

    if (odfs_media_open_image(IMG_RR, &inner) != ODFS_OK) {
        printf("  (skipped: fixture " IMG_RR " unavailable)\n");
        return;
    }

    odfs_mount_opts_default(&opts);
    ASSERT_OK(odfs_mount(&media, &opts, NULL, &mnt));
    ASSERT_EQ(mnt.active_backend, ODFS_BACKEND_ROCK_RIDGE);
    odfs_unmount(&mnt);

    /* HFSFIRST tries HFS+ first and falls back when it cannot mount */
    opts.prefer_hfs = 1;
    ASSERT_OK(odfs_mount(&media, &opts, NULL, &mnt));
    ASSERT_EQ(mnt.active_backend, ODFS_BACKEND_ROCK_RIDGE);
    odfs_unmount(&mnt);

    odfs_media_close(&inner);
}

#define IMG_PLAIN "tests/images/test_plain.iso"
#define IMG_RR_JOLIET "tests/images/test_rr_joliet.iso"

/* on success, odfs_unmount() closes the media */
static int mount_image(const char *path, const odfs_mount_opts_t *opts,
                       odfs_media_t *media, odfs_mount_t *mnt)
{
    if (odfs_media_open_image(path, media) != ODFS_OK) {
        printf("  (skipped: fixture %s unavailable)\n", path);
        return 0;
    }
    if (odfs_mount(media, opts, NULL, mnt) != ODFS_OK) {
        odfs_media_close(media);
        return -1;
    }
    return 1;
}

static odfs_err_t find_name_cb(const odfs_node_t *entry, void *ctx)
{
    if (strcmp(entry->name, ctx) == 0)
        return ODFS_ERR_EOF;
    return ODFS_OK;
}

TEST(mount_applies_iso_options)
{
    odfs_media_t media;
    odfs_mount_opts_t opts;
    odfs_mount_t mnt;
    int rc;

    /* NORR on a Rock Ridge disc gives the plain ISO 9660 view */
    odfs_mount_opts_default(&opts);
    opts.disable_rr = 1;
    rc = mount_image(IMG_RR, &opts, &media, &mnt);
    if (rc == 0)
        return;
    ASSERT_EQ(rc, 1);
    ASSERT_EQ(mnt.active_backend, ODFS_BACKEND_ISO9660);
    odfs_unmount(&mnt);

    /* ... and on a Rock Ridge + Joliet disc the Joliet view */
    rc = mount_image(IMG_RR_JOLIET, &opts, &media, &mnt);
    if (rc == 0)
        return;
    ASSERT_EQ(rc, 1);
    ASSERT_EQ(mnt.active_backend, ODFS_BACKEND_JOLIET);
    odfs_unmount(&mnt);

    /* LOWERCASE lowercases plain ISO 9660 names */
    odfs_mount_opts_default(&opts);
    opts.lowercase_iso = 1;
    rc = mount_image(IMG_PLAIN, &opts, &media, &mnt);
    if (rc == 0)
        return;
    ASSERT_EQ(rc, 1);
    ASSERT_EQ(mnt.active_backend, ODFS_BACKEND_ISO9660);
    ASSERT_ERR(odfs_readdir(&mnt, &mnt.root, find_name_cb, "deep", NULL),
               ODFS_ERR_EOF);
    odfs_unmount(&mnt);
}

TEST(mount_force_backend_selects_rock_ridge)
{
    odfs_media_t media;
    odfs_mount_opts_t opts;
    odfs_mount_t mnt;
    int rc;

    odfs_mount_opts_default(&opts);
    opts.force_backend = ODFS_BACKEND_ROCK_RIDGE;
    rc = mount_image(IMG_RR, &opts, &media, &mnt);
    if (rc == 0)
        return;
    ASSERT_EQ(rc, 1);
    ASSERT_EQ(mnt.active_backend, ODFS_BACKEND_ROCK_RIDGE);
    odfs_unmount(&mnt);

    /* a disc without Rock Ridge does not mount */
    rc = mount_image(IMG_PLAIN, &opts, &media, &mnt);
    if (rc == 0)
        return;
    ASSERT_EQ(rc, -1);

    /* forcing ISO 9660 still detects Rock Ridge inside it */
    opts.force_backend = ODFS_BACKEND_ISO9660;
    rc = mount_image(IMG_RR, &opts, &media, &mnt);
    if (rc == 0)
        return;
    ASSERT_EQ(rc, 1);
    ASSERT_EQ(mnt.active_backend, ODFS_BACKEND_ROCK_RIDGE);
    odfs_unmount(&mnt);
}

TEST(mount_rejects_missing_forced_session)
{
    odfs_media_t media;
    odfs_mount_opts_t opts;
    odfs_mount_t mnt;
    int rc;

    /* test_plain.iso has no TOC: session 0 is the whole image */
    odfs_mount_opts_default(&opts);
    opts.force_session = 0;
    rc = mount_image(IMG_PLAIN, &opts, &media, &mnt);
    if (rc == 0)
        return;
    ASSERT_EQ(rc, 1);
    odfs_unmount(&mnt);

    opts.force_session = 1;
    ASSERT_OK(odfs_media_open_image(IMG_PLAIN, &media));
    ASSERT_ERR(odfs_mount(&media, &opts, NULL, &mnt), ODFS_ERR_RANGE);
    odfs_media_close(&media);
}

typedef struct toc_count_media {
    odfs_media_t inner;
    unsigned int toc_reads;
} toc_count_media_t;

static odfs_err_t toc_count_read_sectors(void *ctx, uint32_t lba,
                                         uint32_t count, void *buf)
{
    toc_count_media_t *m = ctx;
    return odfs_media_read(&m->inner, lba, count, buf);
}

static uint32_t toc_count_sector_size(void *ctx)
{
    toc_count_media_t *m = ctx;
    return odfs_media_sector_size(&m->inner);
}

static uint32_t toc_count_sector_count(void *ctx)
{
    toc_count_media_t *m = ctx;
    return odfs_media_sector_count(&m->inner);
}

/* one data track at LBA 0 */
static odfs_err_t toc_count_read_toc(void *ctx, odfs_toc_t *toc)
{
    toc_count_media_t *m = ctx;

    m->toc_reads++;
    memset(toc, 0, sizeof(*toc));
    toc->first_session = 1;
    toc->last_session = 1;
    toc->session_count = 1;
    toc->sessions[0].number = 1;
    toc->sessions[0].control = 0x04;
    return ODFS_OK;
}

TEST(mount_reads_toc_once)
{
    static const odfs_media_ops_t toc_count_ops = {
        .read_sectors = toc_count_read_sectors,
        .sector_size  = toc_count_sector_size,
        .sector_count = toc_count_sector_count,
        .read_toc     = toc_count_read_toc,
    };
    toc_count_media_t tm;
    odfs_media_t media = { &toc_count_ops, &tm };
    odfs_mount_t mnt;

    if (odfs_media_open_image(IMG_RR, &tm.inner) != ODFS_OK) {
        printf("  (skipped: fixture " IMG_RR " unavailable)\n");
        return;
    }
    tm.toc_reads = 0;

    ASSERT_OK(odfs_mount(&media, NULL, NULL, &mnt));
    ASSERT_EQ(tm.toc_reads, 1);
    odfs_unmount(&mnt);
    odfs_media_close(&tm.inner);
}

TEST_MAIN()
