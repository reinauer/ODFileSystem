/*
 * imgls — list directories in an image
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "odfs/api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static odfs_err_t print_entry(const odfs_node_t *entry, void *ctx)
{
    (void)ctx;
    const char *kind = odfs_node_kind_name(entry->kind);
    printf("%-7s %10llu  %s\n", kind, (unsigned long long)entry->size, entry->name);
    return ODFS_OK;
}

int main(int argc, char **argv)
{
    odfs_media_t media;
    odfs_mount_t mnt;
    odfs_mount_opts_t opts;
    odfs_log_state_t log;
    odfs_err_t err;
    const char *image = NULL;
    const char *path = "/";
    int force_udf = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-u") == 0 || strcmp(argv[i], "--udf") == 0)
            force_udf = 1;
        else if (!image)
            image = argv[i];
        else
            path = argv[i];
    }

    if (!image) {
        fprintf(stderr, "usage: imgls [-u] <image> [path]\n");
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

    odfs_node_t dir;
    err = odfs_resolve_path(&mnt, path, &dir);
    if (err != ODFS_OK) {
        fprintf(stderr, "error: path '%s': %s\n", path, odfs_err_str(err));
        odfs_unmount(&mnt);
        return 1;
    }

    err = odfs_readdir(&mnt, &dir, print_entry, NULL, NULL);
    if (err != ODFS_OK) {
        fprintf(stderr, "error: readdir: %s\n", odfs_err_str(err));
        odfs_unmount(&mnt);
        return 1;
    }

    odfs_unmount(&mnt);
    return 0;
}
