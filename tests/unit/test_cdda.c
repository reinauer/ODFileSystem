/*
 * test_cdda.c — tests for CDDA virtual track handling
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "cdda/cdda.h"
#include "odfs/error.h"
#include "test_harness.h"

#include <string.h>

typedef struct collect_ctx {
    int count;
    odfs_node_t entries[8];
} collect_ctx_t;

static odfs_err_t collect_entry(const odfs_node_t *entry, void *ctx)
{
    collect_ctx_t *collect = ctx;

    if (collect->count >= 8)
        return ODFS_ERR_NOMEM;

    collect->entries[collect->count++] = *entry;
    return ODFS_OK;
}

TEST(cdda_mixed_mode_exports_only_audio_tracks)
{
    odfs_toc_t toc;
    odfs_node_t root;
    void *backend_ctx = NULL;
    cdda_context_t *cdda_ctx;
    collect_ctx_t collect;

    memset(&toc, 0, sizeof(toc));
    toc.session_count = 3;
    toc.sessions[0].number = 1;
    toc.sessions[0].control = 0x00;
    toc.sessions[0].start_lba = 0;
    toc.sessions[1].number = 2;
    toc.sessions[1].control = 0x00;
    toc.sessions[1].start_lba = 100;
    toc.sessions[2].number = 8;
    toc.sessions[2].control = 0x04;
    toc.sessions[2].start_lba = 200;

    ASSERT_OK(cdda_mount_from_toc(&toc, 1, NULL, &root, &backend_ctx));
    cdda_ctx = backend_ctx;
    ASSERT_EQ(cdda_ctx->track_count, 2);

    memset(&collect, 0, sizeof(collect));
    ASSERT_OK(cdda_backend_ops.readdir(backend_ctx, NULL, NULL, &root,
                                       collect_entry, &collect, NULL));

    ASSERT_EQ(collect.count, 2);
    ASSERT_STR_EQ(collect.entries[0].name, "Track01.wav");
    ASSERT_STR_EQ(collect.entries[1].name, "Track02.wav");
    ASSERT_EQ(collect.entries[0].id, 1);
    ASSERT_EQ(collect.entries[1].id, 2);
    ASSERT_EQ(collect.entries[0].extent.lba, 2);
    ASSERT_EQ(collect.entries[1].extent.lba, 3);

    cdda_backend_ops.unmount(backend_ctx);
}

TEST_MAIN()
