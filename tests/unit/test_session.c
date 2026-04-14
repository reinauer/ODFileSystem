/*
 * test_session.c — tests for multisession session selection
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "odfs/api.h"
#include "odfs/error.h"
#include "test_harness.h"

#include <string.h>

typedef struct fake_session_media {
    odfs_toc_t toc;
    odfs_err_t toc_err;
    uint32_t last_session_lba;
    odfs_err_t last_session_err;
} fake_session_media_t;

static odfs_err_t fake_read_sectors(void *ctx, uint32_t lba,
                                    uint32_t count, void *buf)
{
    (void)ctx;
    (void)lba;
    (void)count;
    (void)buf;
    return ODFS_ERR_UNSUPPORTED;
}

static uint32_t fake_sector_size(void *ctx)
{
    (void)ctx;
    return 2048;
}

static uint32_t fake_sector_count(void *ctx)
{
    (void)ctx;
    return 0;
}

static odfs_err_t fake_read_toc(void *ctx, odfs_toc_t *toc)
{
    fake_session_media_t *media = ctx;

    if (media->toc_err != ODFS_OK)
        return media->toc_err;

    *toc = media->toc;
    return ODFS_OK;
}

static odfs_err_t fake_read_last_session_lba(void *ctx, uint32_t *lba_out)
{
    fake_session_media_t *media = ctx;

    if (media->last_session_err != ODFS_OK)
        return media->last_session_err;

    *lba_out = media->last_session_lba;
    return ODFS_OK;
}

static const odfs_media_ops_t fake_media_ops = {
    .read_sectors          = fake_read_sectors,
    .sector_size           = fake_sector_size,
    .sector_count          = fake_sector_count,
    .read_toc              = fake_read_toc,
    .read_last_session_lba = fake_read_last_session_lba,
    .read_audio            = NULL,
    .close                 = NULL,
};

TEST(find_last_session_prefers_explicit_last_session_query)
{
    fake_session_media_t fake;
    odfs_media_t media;
    uint32_t lba = 999;

    memset(&fake, 0, sizeof(fake));
    fake.toc_err = ODFS_OK;
    fake.last_session_err = ODFS_OK;
    fake.last_session_lba = 452;
    fake.toc.session_count = 3;
    fake.toc.sessions[0].number = 1;
    fake.toc.sessions[0].control = 0x00;
    fake.toc.sessions[0].start_lba = 0;
    fake.toc.sessions[1].number = 2;
    fake.toc.sessions[1].control = 0x04;
    fake.toc.sessions[1].start_lba = 452;
    fake.toc.sessions[2].number = 3;
    fake.toc.sessions[2].control = 0x00;
    fake.toc.sessions[2].start_lba = 5678;

    media.ops = &fake_media_ops;
    media.ctx = &fake;

    ASSERT_OK(odfs_find_last_session(&media, NULL, &lba));
    ASSERT_EQ(lba, 452);
}

TEST(find_last_session_uses_first_data_track_in_explicit_session)
{
    fake_session_media_t fake;
    odfs_media_t media;
    uint32_t lba = 999;

    memset(&fake, 0, sizeof(fake));
    fake.toc_err = ODFS_OK;
    fake.last_session_err = ODFS_OK;
    fake.last_session_lba = 0;
    fake.toc.session_count = 3;
    fake.toc.sessions[0].number = 1;
    fake.toc.sessions[0].control = 0x00;
    fake.toc.sessions[0].start_lba = 0;
    fake.toc.sessions[1].number = 2;
    fake.toc.sessions[1].control = 0x04;
    fake.toc.sessions[1].start_lba = 1234;
    fake.toc.sessions[2].number = 3;
    fake.toc.sessions[2].control = 0x00;
    fake.toc.sessions[2].start_lba = 5678;

    media.ops = &fake_media_ops;
    media.ctx = &fake;

    ASSERT_OK(odfs_find_last_session(&media, NULL, &lba));
    ASSERT_EQ(lba, 1234);
}

TEST(find_last_session_keeps_track_one_data_for_kodak_style_disc)
{
    fake_session_media_t fake;
    odfs_media_t media;
    uint32_t lba = 999;

    memset(&fake, 0, sizeof(fake));
    fake.toc_err = ODFS_OK;
    fake.last_session_err = ODFS_OK;
    fake.last_session_lba = 0;
    fake.toc.session_count = 2;
    fake.toc.sessions[0].number = 1;
    fake.toc.sessions[0].control = 0x04;
    fake.toc.sessions[0].start_lba = 0;
    fake.toc.sessions[1].number = 2;
    fake.toc.sessions[1].control = 0x04;
    fake.toc.sessions[1].start_lba = 452;

    media.ops = &fake_media_ops;
    media.ctx = &fake;

    ASSERT_OK(odfs_find_last_session(&media, NULL, &lba));
    ASSERT_EQ(lba, 0);
}

TEST(find_last_session_falls_back_to_last_data_track_heuristic)
{
    fake_session_media_t fake;
    odfs_media_t media;
    uint32_t lba = 999;

    memset(&fake, 0, sizeof(fake));
    fake.toc_err = ODFS_OK;
    fake.last_session_err = ODFS_ERR_UNSUPPORTED;
    fake.toc.session_count = 3;
    fake.toc.sessions[0].number = 1;
    fake.toc.sessions[0].control = 0x00;
    fake.toc.sessions[0].start_lba = 0;
    fake.toc.sessions[1].number = 2;
    fake.toc.sessions[1].control = 0x04;
    fake.toc.sessions[1].start_lba = 1234;
    fake.toc.sessions[2].number = 3;
    fake.toc.sessions[2].control = 0x00;
    fake.toc.sessions[2].start_lba = 5678;

    media.ops = &fake_media_ops;
    media.ctx = &fake;

    ASSERT_OK(odfs_find_last_session(&media, NULL, &lba));
    ASSERT_EQ(lba, 1234);
}

TEST(find_last_session_keeps_lba_zero_for_audio_only_toc)
{
    fake_session_media_t fake;
    odfs_media_t media;
    uint32_t lba = 999;

    memset(&fake, 0, sizeof(fake));
    fake.toc_err = ODFS_OK;
    fake.last_session_err = ODFS_ERR_UNSUPPORTED;
    fake.toc.session_count = 2;
    fake.toc.sessions[0].number = 1;
    fake.toc.sessions[0].control = 0x00;
    fake.toc.sessions[0].start_lba = 0;
    fake.toc.sessions[1].number = 2;
    fake.toc.sessions[1].control = 0x00;
    fake.toc.sessions[1].start_lba = 25810;

    media.ops = &fake_media_ops;
    media.ctx = &fake;

    ASSERT_OK(odfs_find_last_session(&media, NULL, &lba));
    ASSERT_EQ(lba, 0);
}

TEST_MAIN()
