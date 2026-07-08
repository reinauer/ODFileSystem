/*
 * test_file_media.c — tests for host image media helpers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "odfs/error.h"
#include "odfs/media.h"
#include "cdda/cdda.h"
#include "test_harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define TEST_RAW_SECTOR_SIZE 2352
#define TEST_DATA_OFFSET 24

static int test_make_temp_dir(char *buf, size_t buf_size)
{
    static unsigned int counter = 0;

    counter++;
    if (snprintf(buf, buf_size, "/tmp/odfs_file_media_%ld_%lu_%u",
                 (long)getpid(), (unsigned long)time(NULL), counter) >=
        (int)buf_size)
        return 0;

    return mkdir(buf, 0700) == 0;
}

static int test_write_file(const char *path, const void *buf, size_t len)
{
    FILE *fp = fopen(path, "wb");

    if (!fp)
        return 0;
    if (fwrite(buf, 1, len, fp) != len) {
        fclose(fp);
        return 0;
    }
    return fclose(fp) == 0;
}

TEST(cue_mode2_2352_reads_cooked_sectors)
{
    char dir_path[256];
    char cue_path[320];
    char bin_path[320];
    uint8_t raw[4 * TEST_RAW_SECTOR_SIZE];
    uint8_t cooked[4 * 2048];
    odfs_media_t media;
    odfs_err_t err;
    size_t i;

    ASSERT(test_make_temp_dir(dir_path, sizeof(dir_path)));
    ASSERT(snprintf(cue_path, sizeof(cue_path), "%s/disc.cue", dir_path) <
           (int)sizeof(cue_path));
    ASSERT(snprintf(bin_path, sizeof(bin_path), "%s/disc.bin", dir_path) <
           (int)sizeof(bin_path));

    memset(raw, 0, sizeof(raw));
    for (i = 0; i < 4; i++) {
        memset(raw + i * TEST_RAW_SECTOR_SIZE + TEST_DATA_OFFSET,
               0x10 + (int)i, 2048);
    }

    ASSERT(test_write_file(bin_path, raw, sizeof(raw)));
    ASSERT(test_write_file(cue_path,
                           "FILE \"disc.bin\" BINARY\n"
                           "  TRACK 01 MODE2/2352\n"
                           "    INDEX 01 00:00:00\n"
                           "  TRACK 02 MODE2/2352\n"
                           "    INDEX 01 00:00:02\n",
                           strlen("FILE \"disc.bin\" BINARY\n"
                                  "  TRACK 01 MODE2/2352\n"
                                  "    INDEX 01 00:00:00\n"
                                  "  TRACK 02 MODE2/2352\n"
                                  "    INDEX 01 00:00:02\n")));

    err = odfs_media_open_image(cue_path, &media);
    ASSERT_OK(err);
    ASSERT_EQ(odfs_media_sector_size(&media), 2048);
    ASSERT_EQ(odfs_media_sector_count(&media), 4);

    err = odfs_media_read(&media, 0, 4, cooked);
    ASSERT_OK(err);
    for (i = 0; i < 4; i++) {
        ASSERT_EQ(cooked[i * 2048], 0x10 + (int)i);
        ASSERT_EQ(cooked[i * 2048 + 2047], 0x10 + (int)i);
    }

    odfs_media_close(&media);

    ASSERT(remove(cue_path) == 0);
    ASSERT(remove(bin_path) == 0);
    ASSERT(rmdir(dir_path) == 0);
}

#define TEST_AUDIO_FRAMES 4
#define TEST_AUDIO_FRAME_SIZE 2352

typedef struct test_dir_entry {
    char    name[ODFS_NAME_MAX];
    char    comment[ODFS_AMIGA_COMMENT_MAX];
    uint8_t has_comment;
} test_dir_entry_t;

typedef struct test_collect {
    test_dir_entry_t entries[8];
    int              count;
} test_collect_t;

static odfs_err_t test_collect_entry(const odfs_node_t *entry, void *ctx)
{
    test_collect_t *c = ctx;

    if (c->count < (int)(sizeof(c->entries) / sizeof(c->entries[0]))) {
        snprintf(c->entries[c->count].name,
                 sizeof(c->entries[c->count].name), "%s", entry->name);
        snprintf(c->entries[c->count].comment,
                 sizeof(c->entries[c->count].comment), "%s",
                 entry->amiga_as.comment);
        c->entries[c->count].has_comment = entry->amiga_as.has_comment;
        c->count++;
    }
    return ODFS_OK;
}

static void test_put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/*
 * An EAC-style audio rip: cue sheet with CD-Text titles backed by a
 * WAV whose data chunk sits behind the fmt chunk and an odd-sized
 * junk chunk. The titles must come back as AmigaDOS file comments
 * through the real CD-Text pack decoder.
 */
TEST(cue_audio_wav_exposes_cdtext_comments)
{
    char dir_path[256];
    char cue_path[320];
    char wav_path[320];
    uint8_t wav[12 + 8 + 16 + 8 + 6 + 8 +
                TEST_AUDIO_FRAMES * TEST_AUDIO_FRAME_SIZE];
    uint8_t frame[TEST_AUDIO_FRAME_SIZE];
    uint8_t *p = wav;
    uint8_t *cdtext = NULL;
    size_t cdtext_len = 0;
    char buf[512];
    size_t len = sizeof(buf) - 1;
    odfs_media_t media;
    odfs_toc_t toc;
    odfs_node_t root;
    odfs_node_t node;
    odfs_mount_opts_t opts;
    void *backend_ctx = NULL;
    test_collect_t collect;
    odfs_err_t err;
    size_t i;

    ASSERT(test_make_temp_dir(dir_path, sizeof(dir_path)));
    ASSERT(snprintf(cue_path, sizeof(cue_path), "%s/disc.cue", dir_path) <
           (int)sizeof(cue_path));
    ASSERT(snprintf(wav_path, sizeof(wav_path), "%s/disc.wav", dir_path) <
           (int)sizeof(wav_path));

    memcpy(p, "RIFF", 4);
    test_put_le32(p + 4, (uint32_t)(sizeof(wav) - 8));
    memcpy(p + 8, "WAVE", 4);
    p += 12;
    memcpy(p, "fmt ", 4);
    test_put_le32(p + 4, 16);
    memset(p + 8, 0, 16);
    p[8] = 1;   /* PCM */
    p[10] = 2;  /* stereo */
    test_put_le32(p + 12, 44100);
    p += 8 + 16;
    memcpy(p, "JUNK", 4);
    test_put_le32(p + 4, 5); /* odd size: chunks are word-aligned */
    memset(p + 8, 0xee, 6);
    p += 8 + 6;
    memcpy(p, "data", 4);
    test_put_le32(p + 4, TEST_AUDIO_FRAMES * TEST_AUDIO_FRAME_SIZE);
    p += 8;
    for (i = 0; i < TEST_AUDIO_FRAMES; i++) {
        memset(p, 0xa0 + (int)i, TEST_AUDIO_FRAME_SIZE);
        p += TEST_AUDIO_FRAME_SIZE;
    }

    ASSERT(test_write_file(wav_path, wav, sizeof(wav)));

    {
        const char *cue =
            "PERFORMER \"Queen\"\n"
            "TITLE \"A Kind Of Magic\"\n"
            "FILE \"disc.wav\" WAVE\n"
            "  TRACK 01 AUDIO\n"
            "    TITLE \"One Vision (Extended Vision)\"\n"
            "    PERFORMER \"Queen\"\n"
            "    REM COMPOSER \"\"\n"
            "    INDEX 01 00:00:00\n"
            "  TRACK 02 AUDIO\n"
            "    TITLE \"Forever\"\n"
            "    INDEX 01 00:00:02\n";

        ASSERT(test_write_file(cue_path, cue, strlen(cue)));
    }

    err = odfs_media_open_image(cue_path, &media);
    ASSERT_OK(err);
    ASSERT_EQ(odfs_media_sector_count(&media), TEST_AUDIO_FRAMES);

    err = odfs_media_read_toc(&media, &toc);
    ASSERT_OK(err);
    ASSERT_EQ(toc.session_count, 2);
    ASSERT_EQ(toc.sessions[0].number, 1);
    ASSERT_EQ(toc.sessions[0].control, 0x00);
    ASSERT_EQ(toc.sessions[0].start_lba, 0);
    ASSERT_EQ(toc.sessions[0].length, 2);
    ASSERT_EQ(toc.sessions[1].start_lba, 2);
    ASSERT_EQ(toc.leadout_lba, TEST_AUDIO_FRAMES);

    /* frames come from the WAV data chunk, not the file start */
    err = odfs_media_read_audio(&media, 1, 1, frame);
    ASSERT_OK(err);
    ASSERT_EQ(frame[0], 0xa1);
    ASSERT_EQ(frame[TEST_AUDIO_FRAME_SIZE - 1], 0xa1);

    err = odfs_media_read_cdtext(&media, &cdtext, &cdtext_len);
    ASSERT_OK(err);
    ASSERT(cdtext_len > 4);
    ASSERT_EQ((cdtext_len - 4) % 18, 0);
    free(cdtext);

    odfs_mount_opts_default(&opts);
    ASSERT_OK(cdda_mount_from_toc(&toc, 0, &opts, &media,
                                  &root, &backend_ctx));

    memset(&collect, 0, sizeof(collect));
    ASSERT_OK(cdda_backend_ops.readdir(backend_ctx, NULL, NULL, &root,
                                       test_collect_entry, &collect, NULL));
    ASSERT_EQ(collect.count, 4);
    ASSERT_STR_EQ(collect.entries[0].name, "CDDB.txt");
    ASSERT_STR_EQ(collect.entries[1].name, "CD-TEXT.txt");
    ASSERT_STR_EQ(collect.entries[2].name, "Track01.wav");
    ASSERT_STR_EQ(collect.entries[3].name, "Track02.wav");

    ASSERT_EQ(collect.entries[2].has_comment, 1);
    ASSERT_STR_EQ(collect.entries[2].comment,
                  "One Vision (Extended Vision)");
    ASSERT_EQ(collect.entries[3].has_comment, 1);
    ASSERT_STR_EQ(collect.entries[3].comment, "Forever");

    ASSERT_OK(cdda_backend_ops.lookup(backend_ctx, NULL, NULL, &root,
                                      "CD-TEXT.txt", &node));
    ASSERT_OK(cdda_backend_ops.read(backend_ctx, NULL, NULL, &node, 0,
                                    buf, &len));
    buf[len] = '\0';
    ASSERT(strstr(buf, "DISC.TITLE=A Kind Of Magic\n") != NULL);
    ASSERT(strstr(buf, "DISC.PERFORMER=Queen\n") != NULL);
    ASSERT(strstr(buf,
                  "TRACK01.TITLE=One Vision (Extended Vision)\n") != NULL);
    ASSERT(strstr(buf, "TRACK02.TITLE=Forever\n") != NULL);
    ASSERT(strstr(buf, "TRACK01.PERFORMER=Queen\n") != NULL);
    /* track 2 has no PERFORMER line: nothing must be invented */
    ASSERT(strstr(buf, "TRACK02.PERFORMER") == NULL);

    /* the disc title names the volume */
    {
        char volname[64];

        ASSERT_OK(cdda_backend_ops.get_volume_name(backend_ctx, volname,
                                                   sizeof(volname)));
        ASSERT_STR_EQ(volname, "A Kind Of Magic");
    }

    cdda_backend_ops.unmount(backend_ctx);
    odfs_media_close(&media);

    ASSERT(remove(cue_path) == 0);
    ASSERT(remove(wav_path) == 0);
    ASSERT(rmdir(dir_path) == 0);
}

TEST_MAIN()
