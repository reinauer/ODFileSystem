/*
 * test_file_media.c — tests for host image media helpers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "odfs/error.h"
#include "odfs/media.h"
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

TEST_MAIN()
