/*
 * test_cdda.c — tests for CDDA virtual track handling
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "cdda/cdda.h"
#include "odfs/alloc.h"
#include "odfs/error.h"
#include "test_harness.h"

#include <string.h>

typedef struct collect_ctx {
    int count;
    odfs_node_t entries[8];
} collect_ctx_t;

static odfs_err_t fake_read_audio(void *ctx, uint32_t lba,
                                  uint32_t count, void *buf)
{
    uint8_t *src = ctx;
    uint8_t *dst = buf;

    if (lba != 0 || count != 1)
        return ODFS_ERR_INVAL;
    memcpy(dst, src, CDDA_FRAME_SIZE);
    return ODFS_OK;
}

static odfs_err_t fake_read_cdtext(void *ctx, uint8_t **buf_out, size_t *len_out)
{
    static const uint8_t response[] = {
        0x00, 0x38, 0x00, 0x00,
        0x80, 0x00, 0x00, 0x00,
        'A', 'l', 'b', 'u', 'm', 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00,
        0x81, 0x00, 0x01, 0x00,
        'A', 'r', 't', 'i', 's', 't', 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00,
        0x80, 0x01, 0x02, 0x00,
        'S', 'o', 'n', 'g', ' ', '1', 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00
    };
    uint8_t *buf;

    (void)ctx;

    buf = odfs_malloc(sizeof(response));
    if (!buf)
        return ODFS_ERR_NOMEM;
    memcpy(buf, response, sizeof(response));
    *buf_out = buf;
    *len_out = sizeof(response);
    return ODFS_OK;
}

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

    ASSERT_OK(cdda_mount_from_toc(&toc, 1, NULL, NULL, &root, &backend_ctx));
    cdda_ctx = backend_ctx;
    ASSERT_EQ(cdda_ctx->track_count, 2);

    memset(&collect, 0, sizeof(collect));
    ASSERT_OK(cdda_backend_ops.readdir(backend_ctx, NULL, NULL, &root,
                                       collect_entry, &collect, NULL));

    ASSERT_EQ(collect.count, 3);
    ASSERT_STR_EQ(collect.entries[0].name, "CDDB.txt");
    ASSERT_STR_EQ(collect.entries[1].name, "Track01.wav");
    ASSERT_STR_EQ(collect.entries[2].name, "Track02.wav");
    ASSERT_EQ(collect.entries[1].id, 1);
    ASSERT_EQ(collect.entries[2].id, 2);
    ASSERT_EQ(collect.entries[1].extent.lba, 2);
    ASSERT_EQ(collect.entries[2].extent.lba, 3);

    cdda_backend_ops.unmount(backend_ctx);
}

TEST(cdda_pure_audio_uses_leadout_for_last_track)
{
    odfs_toc_t toc;
    odfs_node_t root;
    void *backend_ctx = NULL;
    cdda_context_t *cdda_ctx;
    collect_ctx_t collect;

    memset(&toc, 0, sizeof(toc));
    toc.session_count = 2;
    toc.leadout_lba = 300;
    toc.sessions[0].number = 1;
    toc.sessions[0].control = 0x00;
    toc.sessions[0].start_lba = 0;
    toc.sessions[0].length = 100;
    toc.sessions[1].number = 2;
    toc.sessions[1].control = 0x00;
    toc.sessions[1].start_lba = 100;

    ASSERT_OK(cdda_mount_from_toc(&toc, 0, NULL, NULL, &root, &backend_ctx));
    cdda_ctx = backend_ctx;
    ASSERT_EQ(cdda_ctx->track_count, 2);
    ASSERT_EQ(cdda_ctx->tracks[1].length_frames, 200);

    memset(&collect, 0, sizeof(collect));
    ASSERT_OK(cdda_backend_ops.readdir(backend_ctx, NULL, NULL, &root,
                                       collect_entry, &collect, NULL));

    ASSERT_EQ(collect.count, 3);
    ASSERT_STR_EQ(collect.entries[0].name, "CDDB.txt");
    ASSERT_STR_EQ(collect.entries[1].name, "Track01.wav");
    ASSERT_STR_EQ(collect.entries[2].name, "Track02.wav");

    cdda_backend_ops.unmount(backend_ctx);
}

TEST(cdda_cddb_file_exposes_disc_id_and_query)
{
    odfs_toc_t toc;
    odfs_node_t root;
    odfs_node_t cddb;
    void *backend_ctx = NULL;
    char buf[512];
    size_t len = sizeof(buf) - 1;

    memset(&toc, 0, sizeof(toc));
    toc.session_count = 2;
    toc.leadout_lba = 300;
    toc.sessions[0].number = 1;
    toc.sessions[0].control = 0x00;
    toc.sessions[0].start_lba = 0;
    toc.sessions[0].length = 100;
    toc.sessions[1].number = 2;
    toc.sessions[1].control = 0x00;
    toc.sessions[1].start_lba = 100;

    ASSERT_OK(cdda_mount_from_toc(&toc, 0, NULL, NULL, &root, &backend_ctx));
    ASSERT_OK(cdda_backend_ops.lookup(backend_ctx, NULL, NULL, &root,
                                      "CDDB.txt", &cddb));
    ASSERT(cddb.size > 0);

    ASSERT_OK(cdda_backend_ops.read(backend_ctx, NULL, NULL, &cddb, 0,
                                    buf, &len));
    buf[len] = '\0';

    ASSERT(strstr(buf, "DISCID=05000402\n") != NULL);
    ASSERT(strstr(buf, "TRACKS=2\n") != NULL);
    ASSERT(strstr(buf, "TOTAL_SECONDS=4\n") != NULL);
    ASSERT(strstr(buf, "TOTAL_FRAMES=300\n") != NULL);
    ASSERT(strstr(buf, "OFFSET01=150\n") != NULL);
    ASSERT(strstr(buf, "OFFSET02=250\n") != NULL);
    ASSERT(strstr(buf, "QUERY=cddb query 05000402 2 150 250 4\n") != NULL);

    cdda_backend_ops.unmount(backend_ctx);
}

TEST(cdda_aiff_option_changes_names_and_header)
{
    odfs_toc_t toc;
    odfs_node_t root;
    odfs_node_t track;
    odfs_mount_opts_t opts;
    void *backend_ctx = NULL;
    uint8_t hdr[CDDA_AIFF_HEADER_SIZE];
    size_t len = sizeof(hdr);

    memset(&toc, 0, sizeof(toc));
    toc.session_count = 1;
    toc.sessions[0].number = 1;
    toc.sessions[0].control = 0x00;
    toc.sessions[0].start_lba = 0;
    toc.sessions[0].length = 1;

    odfs_mount_opts_default(&opts);
    opts.prefer_aiff = 1;

    ASSERT_OK(cdda_mount_from_toc(&toc, 0, &opts, NULL, &root, &backend_ctx));
    ASSERT_OK(cdda_backend_ops.lookup(backend_ctx, NULL, NULL, &root,
                                      "Track01.aiff", &track));
    ASSERT_STR_EQ(track.name, "Track01.aiff");
    ASSERT_EQ(track.size, CDDA_AIFF_HEADER_SIZE + CDDA_FRAME_SIZE);

    ASSERT_OK(cdda_backend_ops.read(backend_ctx, NULL, NULL, &track, 0,
                                    hdr, &len));
    ASSERT_EQ(len, sizeof(hdr));
    ASSERT(memcmp(hdr, "FORM", 4) == 0);
    ASSERT_EQ(hdr[4], 0x00);
    ASSERT_EQ(hdr[5], 0x00);
    ASSERT_EQ(hdr[6], 0x09);
    ASSERT_EQ(hdr[7], 0x5e);
    ASSERT(memcmp(hdr + 8, "AIFF", 4) == 0);
    ASSERT(memcmp(hdr + 12, "COMM", 4) == 0);
    ASSERT_EQ(hdr[16], 0x00);
    ASSERT_EQ(hdr[17], 0x00);
    ASSERT_EQ(hdr[18], 0x00);
    ASSERT_EQ(hdr[19], 0x12);
    ASSERT_EQ(hdr[20], 0x00);
    ASSERT_EQ(hdr[21], 0x02);
    ASSERT_EQ(hdr[22], 0x00);
    ASSERT_EQ(hdr[23], 0x00);
    ASSERT_EQ(hdr[24], 0x02);
    ASSERT_EQ(hdr[25], 0x4c);
    ASSERT_EQ(hdr[26], 0x00);
    ASSERT_EQ(hdr[27], 0x10);
    ASSERT_EQ(hdr[28], 0x40);
    ASSERT_EQ(hdr[29], 0x0e);
    ASSERT_EQ(hdr[30], 0xac);
    ASSERT_EQ(hdr[31], 0x44);
    ASSERT_EQ(hdr[32], 0x00);
    ASSERT_EQ(hdr[33], 0x00);
    ASSERT_EQ(hdr[34], 0x00);
    ASSERT_EQ(hdr[35], 0x00);
    ASSERT_EQ(hdr[36], 0x00);
    ASSERT_EQ(hdr[37], 0x00);
    ASSERT(memcmp(hdr + 38, "SSND", 4) == 0);
    ASSERT_EQ(hdr[42], 0x00);
    ASSERT_EQ(hdr[43], 0x00);
    ASSERT_EQ(hdr[44], 0x09);
    ASSERT_EQ(hdr[45], 0x38);
    ASSERT_EQ(hdr[46], 0x00);
    ASSERT_EQ(hdr[47], 0x00);
    ASSERT_EQ(hdr[48], 0x00);
    ASSERT_EQ(hdr[49], 0x00);
    ASSERT_EQ(hdr[50], 0x00);
    ASSERT_EQ(hdr[51], 0x00);
    ASSERT_EQ(hdr[52], 0x00);
    ASSERT_EQ(hdr[53], 0x00);

    cdda_backend_ops.unmount(backend_ctx);
}

TEST(cdda_cdtext_file_exposes_parsed_metadata)
{
    odfs_toc_t toc;
    odfs_node_t root;
    odfs_node_t cdtext;
    odfs_mount_opts_t opts;
    void *backend_ctx = NULL;
    char buf[512];
    size_t len = sizeof(buf) - 1;
    odfs_media_ops_t media_ops;
    odfs_media_t media;
    uint8_t frame[CDDA_FRAME_SIZE];
    collect_ctx_t collect;

    memset(&toc, 0, sizeof(toc));
    toc.session_count = 1;
    toc.sessions[0].number = 1;
    toc.sessions[0].control = 0x00;
    toc.sessions[0].start_lba = 0;
    toc.sessions[0].length = 1;

    memset(frame, 0, sizeof(frame));
    memset(&media_ops, 0, sizeof(media_ops));
    media_ops.read_audio = fake_read_audio;
    media_ops.read_cdtext = fake_read_cdtext;
    media.ops = &media_ops;
    media.ctx = frame;

    odfs_mount_opts_default(&opts);

    ASSERT_OK(cdda_mount_from_toc(&toc, 0, &opts, &media,
                                  &root, &backend_ctx));

    memset(&collect, 0, sizeof(collect));
    ASSERT_OK(cdda_backend_ops.readdir(backend_ctx, NULL, NULL, &root,
                                       collect_entry, &collect, NULL));
    ASSERT_EQ(collect.count, 3);
    ASSERT_STR_EQ(collect.entries[0].name, "CDDB.txt");
    ASSERT_STR_EQ(collect.entries[1].name, "CD-TEXT.txt");
    ASSERT_STR_EQ(collect.entries[2].name, "Track01.wav");

    ASSERT_OK(cdda_backend_ops.lookup(backend_ctx, NULL, NULL, &root,
                                      "CD-TEXT.txt", &cdtext));
    ASSERT(cdtext.size > 0);
    ASSERT_OK(cdda_backend_ops.read(backend_ctx, NULL, NULL, &cdtext, 0,
                                    buf, &len));
    buf[len] = '\0';
    ASSERT(strstr(buf, "DISC.TITLE=Album\n") != NULL);
    ASSERT(strstr(buf, "DISC.PERFORMER=Artist\n") != NULL);
    ASSERT(strstr(buf, "TRACK01.TITLE=Song 1\n") != NULL);

    cdda_backend_ops.unmount(backend_ctx);
}

TEST(cdda_aiff_audio_payload_is_big_endian)
{
    odfs_toc_t toc;
    odfs_node_t root;
    odfs_node_t track;
    odfs_mount_opts_t opts;
    void *backend_ctx = NULL;
    uint8_t frame[CDDA_FRAME_SIZE];
    uint8_t audio[8];
    size_t len = sizeof(audio);
    odfs_media_ops_t media_ops;
    odfs_media_t media;

    memset(&toc, 0, sizeof(toc));
    toc.session_count = 1;
    toc.sessions[0].number = 1;
    toc.sessions[0].control = 0x00;
    toc.sessions[0].start_lba = 0;
    toc.sessions[0].length = 1;

    memset(frame, 0, sizeof(frame));
    frame[0] = 0x22;
    frame[1] = 0x11;
    frame[2] = 0x44;
    frame[3] = 0x33;
    frame[4] = 0x66;
    frame[5] = 0x55;
    frame[6] = 0x88;
    frame[7] = 0x77;

    memset(&media_ops, 0, sizeof(media_ops));
    media_ops.read_audio = fake_read_audio;
    media.ops = &media_ops;
    media.ctx = frame;

    odfs_mount_opts_default(&opts);
    opts.prefer_aiff = 1;

    ASSERT_OK(cdda_mount_from_toc(&toc, 0, &opts, &media,
                                  &root, &backend_ctx));
    ASSERT_OK(cdda_backend_ops.lookup(backend_ctx, NULL, NULL, &root,
                                      "Track01.aiff", &track));

    ASSERT_OK(cdda_backend_ops.read(backend_ctx, NULL, NULL, &track,
                                    CDDA_AIFF_HEADER_SIZE, audio, &len));
    ASSERT_EQ(len, sizeof(audio));
    ASSERT_EQ(audio[0], 0x11);
    ASSERT_EQ(audio[1], 0x22);
    ASSERT_EQ(audio[2], 0x33);
    ASSERT_EQ(audio[3], 0x44);
    ASSERT_EQ(audio[4], 0x55);
    ASSERT_EQ(audio[5], 0x66);
    ASSERT_EQ(audio[6], 0x77);
    ASSERT_EQ(audio[7], 0x88);

    cdda_backend_ops.unmount(backend_ctx);
}

TEST_MAIN()
