/*
 * imgdump — debug low-level descriptors/structures
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "odfs/media.h"
#include "odfs/error.h"
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

static void hexdump(const uint8_t *data, size_t len, uint32_t base_offset)
{
    for (size_t i = 0; i < len; i += 16) {
        printf("%08x  ", (unsigned)(base_offset + i));
        for (size_t j = 0; j < 16; j++) {
            if (i + j < len)
                printf("%02x ", data[i + j]);
            else
                printf("   ");
            if (j == 7) printf(" ");
        }
        printf(" |");
        for (size_t j = 0; j < 16 && i + j < len; j++) {
            unsigned char c = data[i + j];
            printf("%c", isprint(c) ? c : '.');
        }
        printf("|\n");
    }
}

int main(int argc, char **argv)
{
    odfs_media_t media;
    odfs_err_t err;
    uint32_t lba = 16; /* default: PVD location */
    uint32_t count = 1;

    if (argc < 2) {
        fprintf(stderr, "usage: imgdump <image.iso> [lba] [count]\n");
        return 1;
    }
    if (argc >= 3) lba = (uint32_t)strtoul(argv[2], NULL, 0);
    if (argc >= 4) count = (uint32_t)strtoul(argv[3], NULL, 0);

    err = odfs_media_open_image(argv[1], &media);
    if (err != ODFS_OK) {
        fprintf(stderr, "error: cannot open '%s': %s\n",
                argv[1], odfs_err_str(err));
        return 1;
    }

    uint32_t ss = odfs_media_sector_size(&media);
    uint8_t *buf = malloc((size_t)ss * count);
    if (!buf) {
        fprintf(stderr, "error: out of memory\n");
        odfs_media_close(&media);
        return 1;
    }

    err = odfs_media_read(&media, lba, count, buf);
    if (err != ODFS_OK) {
        fprintf(stderr, "error: read LBA %u: %s\n", lba, odfs_err_str(err));
        free(buf);
        odfs_media_close(&media);
        return 1;
    }

    printf("LBA %u-%u (%u sector(s), %u bytes/sector)\n",
           lba, lba + count - 1, count, ss);
    hexdump(buf, (size_t)ss * count, lba * ss);

    free(buf);
    odfs_media_close(&media);
    return 0;
}
