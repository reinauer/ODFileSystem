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
    const char *image = NULL;
    const char *path = NULL;
    int force_udf = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-u") == 0 || strcmp(argv[i], "--udf") == 0)
            force_udf = 1;
        else if (!image)
            image = argv[i];
        else
            path = argv[i];
    }

    if (!image || !path) {
        fprintf(stderr, "usage: imgcat [-u] <image> <path>\n");
        return 1;
    }

    err = odfs_media_open_image(image, &media);
    if (err != ODFS_OK) {
        fprintf(stderr, "error: cannot open '%s': %s\n",
                image, odfs_err_str(err));
        return 1;
    }

    odfs_log_init(&log);
    odfs_mount_opts_default(&opts);
    if (force_udf)
        opts.prefer_udf = 1;

    err = odfs_mount(&media, &opts, &log, &mnt);
    if (err != ODFS_OK) {
        fprintf(stderr, "error: mount failed: %s\n", odfs_err_str(err));
        odfs_media_close(&media);
        return 1;
    }

    odfs_node_t node;
    err = odfs_resolve_path(&mnt, path, &node);
    if (err != ODFS_OK) {
        fprintf(stderr, "error: path '%s': %s\n", path, odfs_err_str(err));
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
