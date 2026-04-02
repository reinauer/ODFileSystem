/*
 * imgcat — read/dump file contents from an image
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "odfs/api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    odfs_media_t media;
    odfs_mount_t mnt;
    odfs_mount_opts_t opts;
    odfs_log_state_t log;
    odfs_err_t err;

    if (argc < 3) {
        fprintf(stderr, "usage: imgcat <image.iso> <path>\n");
        return 1;
    }

    err = odfs_media_open_image(argv[1], &media);
    if (err != ODFS_OK) {
        fprintf(stderr, "error: cannot open '%s': %s\n",
                argv[1], odfs_err_str(err));
        return 1;
    }

    odfs_log_init(&log);
    odfs_mount_opts_default(&opts);

    err = odfs_mount(&media, &opts, &log, &mnt);
    if (err != ODFS_OK) {
        fprintf(stderr, "error: mount failed: %s\n", odfs_err_str(err));
        odfs_media_close(&media);
        return 1;
    }

    odfs_node_t node;
    err = odfs_resolve_path(&mnt, argv[2], &node);
    if (err != ODFS_OK) {
        fprintf(stderr, "error: path '%s': %s\n", argv[2], odfs_err_str(err));
        odfs_unmount(&mnt);
        return 1;
    }

    /* read and write to stdout */
    uint8_t buf[8192];
    uint64_t offset = 0;
    while (offset < node.size) {
        size_t len = sizeof(buf);
        if (offset + len > node.size)
            len = (size_t)(node.size - offset);

        err = odfs_read(&mnt, &node, offset, buf, &len);
        if (err != ODFS_OK) {
            fprintf(stderr, "error: read at offset %llu: %s\n",
                    (unsigned long long)offset, odfs_err_str(err));
            break;
        }
        if (len == 0)
            break;

        fwrite(buf, 1, len, stdout);
        offset += len;
    }

    odfs_unmount(&mnt);
    return 0;
}
