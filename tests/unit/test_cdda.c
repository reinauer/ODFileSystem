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

typedef struct fake_audio_ctx {
    const uint8_t *data;
    uint32_t base_lba;
    int calls;
    uint32_t last_lba;
    uint32_t last_count;
} fake_audio_ctx_t;

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

static odfs_err_t fake_read_audio_counted(void *ctx, uint32_t lba,
                                          uint32_t count, void *buf)
{
    fake_audio_ctx_t *audio = ctx;

    audio->calls++;
    audio->last_lba = lba;
    audio->last_count = count;
    memcpy(buf,
           audio->data + (size_t)(lba - audio->base_lba) * CDDA_FRAME_SIZE,
           (size_t)count * CDDA_FRAME_SIZE);
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

TEST(cdda_pure_audio_exports_configured_disk_icon)
{
    static const uint8_t icon_data[] = { 0xe3, 0x10, 0x00, 0x01 };
    odfs_toc_t toc;
    odfs_node_t root;
    odfs_node_t icon;
    void *backend_ctx = NULL;
    cdda_context_t *cdda_ctx;
    collect_ctx_t collect;
    uint8_t buf[sizeof(icon_data)];
    size_t len = sizeof(buf);

    memset(&toc, 0, sizeof(toc));
    toc.session_count = 1;
    toc.leadout_lba = 100;
    toc.sessions[0].number = 1;
    toc.sessions[0].start_lba = 0;

    ASSERT_OK(cdda_mount_from_toc(&toc, 0, NULL, NULL, &root,
                                  &backend_ctx));
    cdda_ctx = backend_ctx;
    cdda_ctx->disk_icon = odfs_malloc(sizeof(icon_data));
    ASSERT(cdda_ctx->disk_icon != NULL);
    memcpy(cdda_ctx->disk_icon, icon_data, sizeof(icon_data));
    cdda_ctx->disk_icon_size = sizeof(icon_data);

    memset(&collect, 0, sizeof(collect));
    ASSERT_OK(cdda_backend_ops.readdir(backend_ctx, NULL, NULL, &root,
                                       collect_entry, &collect, NULL));
    ASSERT_EQ(collect.count, 3);
    ASSERT_STR_EQ(collect.entries[0].name, "Disk.info");

    ASSERT_OK(cdda_backend_ops.lookup(backend_ctx, NULL, NULL, &root,
                                      "disk.info", &icon));
    ASSERT_OK(cdda_backend_ops.read(backend_ctx, NULL, NULL, &icon, 0,
                                    buf, &len));
    ASSERT_EQ(len, sizeof(icon_data));
    ASSERT(memcmp(buf, icon_data, sizeof(icon_data)) == 0);

    cdda_backend_ops.unmount(backend_ctx);
}

TEST(cdda_accepts_red_book_maximum_track_count)
{
    odfs_toc_t toc;
    odfs_node_t root;
    void *backend_ctx = NULL;
    cdda_context_t *cdda_ctx;
    int i;

    memset(&toc, 0, sizeof(toc));
    toc.session_count = CDDA_MAX_TRACKS;
    toc.leadout_lba = CDDA_MAX_TRACKS * 75u;
    for (i = 0; i < CDDA_MAX_TRACKS; i++) {
        toc.sessions[i].number = (uint8_t)(i + 1);
        toc.sessions[i].start_lba = (uint32_t)i * 75u;
        toc.sessions[i].length = 75u;
    }

    ASSERT_OK(cdda_mount_from_toc(&toc, 0, NULL, NULL, &root,
                                  &backend_ctx));
    cdda_ctx = backend_ctx;
    ASSERT_EQ(cdda_ctx->track_count, CDDA_MAX_TRACKS);
    ASSERT_EQ(cdda_ctx->tracks[CDDA_MAX_TRACKS - 1].number,
              CDDA_MAX_TRACKS);
    ASSERT_EQ(cdda_ctx->tracks[CDDA_MAX_TRACKS - 1].length_frames, 75);

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

    ASSERT_STR_EQ(buf,
                  "# Generated by ODFileSystem from disc TOC\n"
                  "DISCID=05000402\n"
                  "TRACKS=2\n"
                  "TOTAL_SECONDS=4\n"
                  "TOTAL_FRAMES=300\n"
                  "OFFSET01=150\n"
                  "OFFSET02=250\n"
                  "QUERY=cddb query 05000402 2 150 250 4\n");

    cdda_backend_ops.unmount(backend_ctx);
}

TEST(cdda_volume_name_uses_disc_id_for_pure_audio)
{
    odfs_toc_t toc;
    odfs_node_t root;
    void *backend_ctx = NULL;
    char name[32];

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
    ASSERT_OK(cdda_backend_ops.get_volume_name(backend_ctx, name,
                                               sizeof(name)));
    ASSERT_STR_EQ(name, "Audio CD (05000402)");

    cdda_backend_ops.unmount(backend_ctx);
}

TEST(cdda_volume_name_keeps_cdda_for_mixed_mode)
{
    odfs_toc_t toc;
    odfs_node_t root;
    void *backend_ctx = NULL;
    char name[32];

    memset(&toc, 0, sizeof(toc));
    toc.session_count = 2;
    toc.sessions[0].number = 1;
    toc.sessions[0].control = 0x00;
    toc.sessions[0].start_lba = 0;
    toc.sessions[0].length = 100;
    toc.sessions[1].number = 2;
    toc.sessions[1].control = 0x04;
    toc.sessions[1].start_lba = 100;

    ASSERT_OK(cdda_mount_from_toc(&toc, 1, NULL, NULL, &root, &backend_ctx));
    ASSERT_OK(cdda_backend_ops.get_volume_name(backend_ctx, name,
                                               sizeof(name)));
    ASSERT_STR_EQ(name, "CDDA");

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
    ASSERT_STR_EQ(buf,
                  "# Generated by ODFileSystem from CD-Text packs\n"
                  "DISC.TITLE=Album\n"
                  "TRACK01.TITLE=Song 1\n"
                  "DISC.PERFORMER=Artist\n");

    /* the track title also becomes the AmigaDOS file comment */
    ASSERT_EQ(collect.entries[2].amiga_as.has_comment, 1);
    ASSERT_STR_EQ(collect.entries[2].amiga_as.comment, "Song 1");
    /* the album title (track 0) must not leak onto track files */
    ASSERT(strcmp(collect.entries[2].amiga_as.comment, "Album") != 0);

    /* ...it names the volume instead of the generic disc-id label */
    {
        char volname[64];

        ASSERT_OK(cdda_backend_ops.get_volume_name(backend_ctx, volname,
                                                   sizeof(volname)));
        ASSERT_STR_EQ(volname, "Album");
    }

    cdda_backend_ops.unmount(backend_ctx);
}

/*
 * Titles flowing across pack boundaries, the TAB "same as previous
 * track" shorthand, and resynchronization via the character-position
 * nibble after zero padding.
 */
static odfs_err_t fake_read_cdtext_flow(void *ctx, uint8_t **buf_out,
                                        size_t *len_out)
{
    static const uint8_t response[] = {
        0x00, 0x62, 0x00, 0x00,
        /* track 1 title spans two packs: "Blue Monday Extended" */
        0x80, 0x01, 0x00, 0x00,
        'B', 'l', 'u', 'e', ' ', 'M', 'o', 'n', 'd', 'a', 'y', ' ',
        0x00, 0x00,
        /* continuation pack: char position 12 */
        0x80, 0x01, 0x01, 0x0c,
        'E', 'x', 't', 'e', 'n', 'd', 'e', 'd', 0x00, '\t', 0x00, 0x00,
        0x00, 0x00,
        /* track 3 restarts cleanly after the padding above */
        0x80, 0x03, 0x02, 0x00,
        'C', 'l', 'o', 's', 'e', 'r', 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
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

TEST(cdda_cdtext_titles_flow_and_tab_shorthand)
{
    odfs_toc_t toc;
    odfs_node_t root;
    odfs_node_t track;
    odfs_mount_opts_t opts;
    void *backend_ctx = NULL;
    odfs_media_ops_t media_ops;
    odfs_media_t media;

    memset(&toc, 0, sizeof(toc));
    toc.session_count = 3;
    toc.sessions[0].number = 1;
    toc.sessions[0].control = 0x00;
    toc.sessions[0].start_lba = 0;
    toc.sessions[0].length = 1;
    toc.sessions[1].number = 2;
    toc.sessions[1].control = 0x00;
    toc.sessions[1].start_lba = 1;
    toc.sessions[1].length = 1;
    toc.sessions[2].number = 3;
    toc.sessions[2].control = 0x00;
    toc.sessions[2].start_lba = 2;
    toc.sessions[2].length = 1;

    memset(&media_ops, 0, sizeof(media_ops));
    media_ops.read_cdtext = fake_read_cdtext_flow;
    media.ops = &media_ops;
    media.ctx = NULL;

    odfs_mount_opts_default(&opts);

    ASSERT_OK(cdda_mount_from_toc(&toc, 0, &opts, &media,
                                  &root, &backend_ctx));

    ASSERT_OK(cdda_backend_ops.lookup(backend_ctx, NULL, NULL, &root,
                                      "Track01.wav", &track));
    ASSERT_EQ(track.amiga_as.has_comment, 1);
    ASSERT_STR_EQ(track.amiga_as.comment, "Blue Monday Extended");

    /* TAB title: same as the previous track */
    ASSERT_OK(cdda_backend_ops.lookup(backend_ctx, NULL, NULL, &root,
                                      "Track02.wav", &track));
    ASSERT_EQ(track.amiga_as.has_comment, 1);
    ASSERT_STR_EQ(track.amiga_as.comment, "Blue Monday Extended");

    ASSERT_OK(cdda_backend_ops.lookup(backend_ctx, NULL, NULL, &root,
                                      "Track03.wav", &track));
    ASSERT_EQ(track.amiga_as.has_comment, 1);
    ASSERT_STR_EQ(track.amiga_as.comment, "Closer");

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

TEST(cdda_audio_reads_use_readahead_cache)
{
    odfs_toc_t toc;
    odfs_node_t root;
    odfs_node_t track;
    void *backend_ctx = NULL;
    uint8_t frames[4 * CDDA_FRAME_SIZE];
    uint8_t audio[4096];
    size_t len = sizeof(audio);
    odfs_media_ops_t media_ops;
    odfs_media_t media;
    fake_audio_ctx_t audio_ctx;

    memset(&toc, 0, sizeof(toc));
    toc.session_count = 1;
    toc.sessions[0].number = 1;
    toc.sessions[0].control = 0x00;
    toc.sessions[0].start_lba = 0;
    toc.sessions[0].length = 4;

    for (size_t i = 0; i < sizeof(frames); i++)
        frames[i] = (uint8_t)i;

    memset(&media_ops, 0, sizeof(media_ops));
    memset(&audio_ctx, 0, sizeof(audio_ctx));
    audio_ctx.data = frames;
    audio_ctx.base_lba = 0;
    media_ops.read_audio = fake_read_audio_counted;
    media.ops = &media_ops;
    media.ctx = &audio_ctx;

    ASSERT_OK(cdda_mount_from_toc(&toc, 0, NULL, &media, &root, &backend_ctx));
    ASSERT_OK(cdda_backend_ops.lookup(backend_ctx, NULL, NULL, &root,
                                      "Track01.wav", &track));

    ASSERT_OK(cdda_backend_ops.read(backend_ctx, NULL, NULL, &track,
                                    CDDA_WAV_HEADER_SIZE, audio, &len));
    ASSERT_EQ(len, sizeof(audio));
    ASSERT_EQ(audio_ctx.calls, 1);
    ASSERT_EQ(audio_ctx.last_lba, 0);
    ASSERT_EQ(audio_ctx.last_count, 4);
    ASSERT_EQ(audio[0], frames[0]);
    ASSERT_EQ(audio[1], frames[1]);
    ASSERT_EQ(audio[4095], frames[4095]);

    len = 512;
    ASSERT_OK(cdda_backend_ops.read(backend_ctx, NULL, NULL, &track,
                                    CDDA_WAV_HEADER_SIZE + 1024u,
                                    audio, &len));
    ASSERT_EQ(len, 512);
    ASSERT_EQ(audio_ctx.calls, 1);
    ASSERT_EQ(audio[0], frames[1024]);
    ASSERT_EQ(audio[511], frames[1024 + 511]);

    cdda_backend_ops.unmount(backend_ctx);
}

TEST(cdda_audio_reads_prefetch_one_second_on_small_reads)
{
    odfs_toc_t toc;
    odfs_node_t root;
    odfs_node_t track;
    void *backend_ctx = NULL;
    uint8_t *frames;
    uint8_t audio[4096];
    size_t len = sizeof(audio);
    odfs_media_ops_t media_ops;
    odfs_media_t media;
    fake_audio_ctx_t audio_ctx;
    size_t frame_bytes = 80u * CDDA_FRAME_SIZE;

    frames = odfs_malloc(frame_bytes);
    ASSERT(frames != NULL);

    memset(&toc, 0, sizeof(toc));
    toc.session_count = 1;
    toc.sessions[0].number = 1;
    toc.sessions[0].control = 0x00;
    toc.sessions[0].start_lba = 0;
    toc.sessions[0].length = 80;

    for (size_t i = 0; i < frame_bytes; i++)
        frames[i] = (uint8_t)i;

    memset(&media_ops, 0, sizeof(media_ops));
    memset(&audio_ctx, 0, sizeof(audio_ctx));
    audio_ctx.data = frames;
    audio_ctx.base_lba = 0;
    media_ops.read_audio = fake_read_audio_counted;
    media.ops = &media_ops;
    media.ctx = &audio_ctx;

    ASSERT_OK(cdda_mount_from_toc(&toc, 0, NULL, &media, &root, &backend_ctx));
    ASSERT_OK(cdda_backend_ops.lookup(backend_ctx, NULL, NULL, &root,
                                      "Track01.wav", &track));

    ASSERT_OK(cdda_backend_ops.read(backend_ctx, NULL, NULL, &track,
                                    CDDA_WAV_HEADER_SIZE, audio, &len));
    ASSERT_EQ(len, sizeof(audio));
    ASSERT_EQ(audio_ctx.calls, 1);
    ASSERT_EQ(audio_ctx.last_lba, 0);
    ASSERT_EQ(audio_ctx.last_count, 75);

    len = 512;
    ASSERT_OK(cdda_backend_ops.read(backend_ctx, NULL, NULL, &track,
                                    CDDA_WAV_HEADER_SIZE + 50000u,
                                    audio, &len));
    ASSERT_EQ(len, 512);
    ASSERT_EQ(audio_ctx.calls, 1);

    len = 512;
    ASSERT_OK(cdda_backend_ops.read(backend_ctx, NULL, NULL, &track,
                                    CDDA_WAV_HEADER_SIZE + 176500u,
                                    audio, &len));
    ASSERT_EQ(len, 512);
    ASSERT_EQ(audio_ctx.calls, 2);
    ASSERT_EQ(audio_ctx.last_lba, 75);
    ASSERT_EQ(audio_ctx.last_count, 5);

    cdda_backend_ops.unmount(backend_ctx);
    odfs_free(frames);
}

static odfs_err_t collect_single_entry(const odfs_node_t *entry, void *ctx)
{
    collect_ctx_t *collect = ctx;

    if (collect->count < 8)
        collect->entries[collect->count++] = *entry;
    return ODFS_ERR_EOF;
}

TEST(cdda_readdir_resume_offset_steps_without_duplicates)
{
    static const uint8_t icon_data[] = { 0xe3, 0x10, 0x00, 0x01 };
    odfs_toc_t toc;
    odfs_node_t root;
    void *backend_ctx = NULL;
    cdda_context_t *cdda_ctx;
    odfs_media_ops_t media_ops;
    odfs_media_t media;
    collect_ctx_t collect;
    uint32_t resume = 0;

    memset(&toc, 0, sizeof(toc));
    toc.session_count = 3;
    toc.leadout_lba = 300;
    for (int i = 0; i < 3; i++) {
        toc.sessions[i].number = (uint8_t)(i + 1);
        toc.sessions[i].start_lba = (uint32_t)i * 100u;
        toc.sessions[i].length = 100u;
    }

    memset(&media_ops, 0, sizeof(media_ops));
    media_ops.read_cdtext = fake_read_cdtext;
    media.ops = &media_ops;
    media.ctx = NULL;

    ASSERT_OK(cdda_mount_from_toc(&toc, 0, NULL, &media, &root, &backend_ctx));
    cdda_ctx = backend_ctx;
    cdda_ctx->disk_icon = odfs_malloc(sizeof(icon_data));
    ASSERT(cdda_ctx->disk_icon != NULL);
    memcpy(cdda_ctx->disk_icon, icon_data, sizeof(icon_data));
    cdda_ctx->disk_icon_size = sizeof(icon_data);

    memset(&collect, 0, sizeof(collect));
    for (int step = 0; step < 6; step++) {
        odfs_err_t err = cdda_backend_ops.readdir(
            backend_ctx, NULL, NULL, &root,
            collect_single_entry, &collect, &resume);
        ASSERT_EQ(err, ODFS_ERR_EOF);
        ASSERT_EQ(resume, (uint32_t)(step + 1));
    }

    ASSERT_OK(cdda_backend_ops.readdir(backend_ctx, NULL, NULL, &root,
                                       collect_single_entry, &collect, &resume));
    ASSERT_EQ(resume, 6u);
    ASSERT_EQ(collect.count, 6);
    ASSERT_STR_EQ(collect.entries[0].name, "Disk.info");
    ASSERT_STR_EQ(collect.entries[1].name, "CDDB.txt");
    ASSERT_STR_EQ(collect.entries[2].name, "CD-TEXT.txt");
    ASSERT_STR_EQ(collect.entries[3].name, "Track01.wav");
    ASSERT_STR_EQ(collect.entries[4].name, "Track02.wav");
    ASSERT_STR_EQ(collect.entries[5].name, "Track03.wav");

    cdda_backend_ops.unmount(backend_ctx);
}

static odfs_err_t fake_read_last_session_lba(void *ctx, uint32_t *lba_out)
{
    *lba_out = *(const uint32_t *)ctx;
    return ODFS_OK;
}

/* Amiga-style TOC: every entry's length already runs to the next start. */
static uint32_t mount_audio_then_data(uint32_t *last_session_lba)
{
    odfs_toc_t toc;
    odfs_node_t root;
    void *backend_ctx = NULL;
    odfs_media_ops_t media_ops;
    odfs_media_t media;
    uint32_t length;

    memset(&toc, 0, sizeof(toc));
    toc.session_count = 2;
    toc.leadout_lba = 25000;
    toc.sessions[0].number = 1;
    toc.sessions[0].start_lba = 0;
    toc.sessions[0].length = 20000;
    toc.sessions[1].number = 2;
    toc.sessions[1].control = 0x04;
    toc.sessions[1].start_lba = 20000;
    toc.sessions[1].length = 5000;

    memset(&media_ops, 0, sizeof(media_ops));
    if (last_session_lba)
        media_ops.read_last_session_lba = fake_read_last_session_lba;
    media.ops = &media_ops;
    media.ctx = last_session_lba;

    if (cdda_mount_from_toc(&toc, 1, NULL, &media, &root,
                            &backend_ctx) != ODFS_OK)
        return 0;
    length = ((cdda_context_t *)backend_ctx)->tracks[0].length_frames;
    cdda_backend_ops.unmount(backend_ctx);
    return length;
}

TEST(cdda_cdextra_last_audio_track_excludes_session_gap)
{
    uint32_t last_session_lba = 20000;

    ASSERT_EQ(mount_audio_then_data(&last_session_lba), 8600u);
}

TEST(cdda_single_session_trailing_data_track_keeps_length)
{
    uint32_t last_session_lba = 0;

    ASSERT_EQ(mount_audio_then_data(&last_session_lba), 20000u);
    ASSERT_EQ(mount_audio_then_data(NULL), 20000u);
}

static odfs_err_t fake_read_cdtext_header_only(void *ctx, uint8_t **buf_out,
                                               size_t *len_out)
{
    static const uint8_t response[] = { 0x00, 0x02, 0x00, 0x00 };

    (void)ctx;
    *buf_out = odfs_malloc(sizeof(response));
    if (!*buf_out)
        return ODFS_ERR_NOMEM;
    memcpy(*buf_out, response, sizeof(response));
    *len_out = sizeof(response);
    return ODFS_OK;
}

TEST(cdda_cdtext_without_packs_has_no_file)
{
    odfs_toc_t toc;
    odfs_node_t root;
    odfs_node_t node;
    void *backend_ctx = NULL;
    odfs_media_ops_t media_ops;
    odfs_media_t media;

    memset(&toc, 0, sizeof(toc));
    toc.session_count = 1;
    toc.sessions[0].number = 1;
    toc.sessions[0].length = 10;

    memset(&media_ops, 0, sizeof(media_ops));
    media_ops.read_cdtext = fake_read_cdtext_header_only;
    media.ops = &media_ops;
    media.ctx = NULL;

    ASSERT_OK(cdda_mount_from_toc(&toc, 0, NULL, &media, &root, &backend_ctx));
    ASSERT_EQ(cdda_backend_ops.lookup(backend_ctx, NULL, NULL, &root,
                                      "CD-TEXT.txt", &node),
              ODFS_ERR_NOT_FOUND);
    cdda_backend_ops.unmount(backend_ctx);
}

static odfs_err_t fake_read_cdtext_dense_short(void *ctx, uint8_t **buf_out,
                                               size_t *len_out)
{
    /*
     * Four packs of six one-character strings render 24 lines of about
     * 30 bytes each, several times the size of the raw packs.
     */
    static const uint8_t response[] = {
        0x00, 0x5a, 0x00, 0x00,
        0x82, 0x01, 0x00, 0x10, /* block 1 SONGWRITER tracks 1..6 */
        'A', 0, 'B', 0, 'C', 0, 'D', 0, 'E', 0, 'F', 0,
        0x00, 0x00,
        0x82, 0x07, 0x01, 0x10, /* block 1 SONGWRITER tracks 7..12 */
        'G', 0, 'H', 0, 'I', 0, 'J', 0, 'K', 0, 'L', 0,
        0x00, 0x00,
        0x83, 0x01, 0x02, 0x10, /* block 1 COMPOSER tracks 1..6 */
        'M', 0, 'N', 0, 'O', 0, 'P', 0, 'Q', 0, 'R', 0,
        0x00, 0x00,
        0x83, 0x07, 0x03, 0x10, /* block 1 COMPOSER tracks 7..12 */
        'S', 0, 'T', 0, 'U', 0, 'V', 0, 'W', 0, 'X', 0,
        0x00, 0x00,
        0x80, 0xfa, 0x04, 0x00, /* track 250 */
        'H', 'i', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
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

TEST(cdda_cdtext_file_holds_dense_short_strings)
{
    odfs_toc_t toc;
    odfs_node_t root;
    odfs_node_t cdtext;
    void *backend_ctx = NULL;
    odfs_media_ops_t media_ops;
    odfs_media_t media;
    char buf[1024];
    size_t len = sizeof(buf) - 1;

    memset(&toc, 0, sizeof(toc));
    toc.session_count = 1;
    toc.sessions[0].number = 1;
    toc.sessions[0].start_lba = 0;
    toc.sessions[0].length = 10;

    memset(&media_ops, 0, sizeof(media_ops));
    media_ops.read_cdtext = fake_read_cdtext_dense_short;
    media.ops = &media_ops;
    media.ctx = NULL;

    ASSERT_OK(cdda_mount_from_toc(&toc, 0, NULL, &media, &root, &backend_ctx));
    ASSERT_OK(cdda_backend_ops.lookup(backend_ctx, NULL, NULL, &root,
                                      "CD-TEXT.txt", &cdtext));
    ASSERT_OK(cdda_backend_ops.read(backend_ctx, NULL, NULL, &cdtext, 0,
                                    buf, &len));
    ASSERT_EQ(len, (size_t)cdtext.size);
    buf[len] = '\0';
    ASSERT(strstr(buf, "BLOCK01.TRACK01.SONGWRITER=A\n") != NULL);
    ASSERT(strstr(buf, "BLOCK01.TRACK12.COMPOSER=X\n") != NULL);
    ASSERT(strstr(buf, "TRACK250.TITLE=Hi\n") != NULL);
    ASSERT_EQ(strlen(buf), len);

    cdda_backend_ops.unmount(backend_ctx);
}

TEST(cdda_audio_cache_is_allocated_on_first_audio_read)
{
    odfs_toc_t toc;
    odfs_node_t root;
    odfs_node_t track;
    void *backend_ctx = NULL;
    cdda_context_t *cdda_ctx;
    uint8_t frames[2 * CDDA_FRAME_SIZE];
    uint8_t buf[64];
    size_t len;
    odfs_media_ops_t media_ops;
    odfs_media_t media;
    fake_audio_ctx_t audio_ctx;

    memset(&toc, 0, sizeof(toc));
    toc.session_count = 1;
    toc.sessions[0].number = 1;
    toc.sessions[0].length = 2;

    for (size_t i = 0; i < sizeof(frames); i++)
        frames[i] = (uint8_t)(i ^ 0x5Au);

    memset(&media_ops, 0, sizeof(media_ops));
    memset(&audio_ctx, 0, sizeof(audio_ctx));
    audio_ctx.data = frames;
    media_ops.read_audio = fake_read_audio_counted;
    media.ops = &media_ops;
    media.ctx = &audio_ctx;

    ASSERT_OK(cdda_mount_from_toc(&toc, 0, NULL, &media, &root, &backend_ctx));
    cdda_ctx = backend_ctx;
    ASSERT(cdda_ctx->audio_cache == NULL);

    ASSERT_OK(cdda_backend_ops.lookup(backend_ctx, NULL, NULL, &root,
                                      "Track01.wav", &track));

    /* the synthesized header needs no audio buffer */
    len = CDDA_WAV_HEADER_SIZE;
    ASSERT_OK(cdda_backend_ops.read(backend_ctx, NULL, NULL, &track,
                                    0, buf, &len));
    ASSERT_EQ(len, CDDA_WAV_HEADER_SIZE);
    ASSERT(cdda_ctx->audio_cache == NULL);
    ASSERT_EQ(audio_ctx.calls, 0);

    len = sizeof(buf);
    ASSERT_OK(cdda_backend_ops.read(backend_ctx, NULL, NULL, &track,
                                    CDDA_WAV_HEADER_SIZE, buf, &len));
    ASSERT_EQ(len, sizeof(buf));
    ASSERT(cdda_ctx->audio_cache != NULL);
    ASSERT_EQ(cdda_ctx->audio_cache_cap, CDDA_READAHEAD_FRAMES);
    ASSERT_EQ(audio_ctx.calls, 1);
    ASSERT(memcmp(buf, frames, sizeof(buf)) == 0);

    cdda_backend_ops.unmount(backend_ctx);
}

TEST(cdda_oversized_toc_length_keeps_header_consistent)
{
    odfs_toc_t toc;
    odfs_node_t root;
    odfs_node_t track;
    void *backend_ctx = NULL;
    uint8_t hdr[CDDA_WAV_HEADER_SIZE];
    size_t len = sizeof(hdr);
    uint32_t riff_size;

    memset(&toc, 0, sizeof(toc));
    toc.session_count = 1;
    toc.sessions[0].number = 1;
    toc.sessions[0].length = 0xffffffffu;

    ASSERT_OK(cdda_mount_from_toc(&toc, 0, NULL, NULL, &root, &backend_ctx));
    ASSERT_OK(cdda_backend_ops.lookup(backend_ctx, NULL, NULL, &root,
                                      "Track01.wav", &track));
    ASSERT_OK(cdda_backend_ops.read(backend_ctx, NULL, NULL, &track, 0,
                                    hdr, &len));
    ASSERT_EQ(len, sizeof(hdr));
    riff_size = (uint32_t)hdr[4] | ((uint32_t)hdr[5] << 8) |
                ((uint32_t)hdr[6] << 16) | ((uint32_t)hdr[7] << 24);
    ASSERT_EQ((uint64_t)riff_size + 8u, track.size);

    cdda_backend_ops.unmount(backend_ctx);
}

TEST_MAIN()
