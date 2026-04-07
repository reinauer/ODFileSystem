/*
 * imgls — list directories in an image
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "odfs/api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct imgls_opts {
    int show_amiga;
} imgls_opts_t;

static odfs_err_t print_entry(const odfs_node_t *entry, void *ctx)
{
    const imgls_opts_t *opts = ctx;
    const char *kind = odfs_node_kind_name(entry->kind);

    printf("%-7s %10llu  %s\n", kind, (unsigned long long)entry->size, entry->name);
    if (opts && opts->show_amiga) {
        if (entry->amiga.has_protection) {
            printf("                     amiga=%02x %02x %02x %02x\n",
                   entry->amiga.protection[0], entry->amiga.protection[1],
                   entry->amiga.protection[2], entry->amiga.protection[3]);
        }
        if (entry->amiga.has_comment && entry->amiga.comment[0] != '\0')
            printf("                     comment=%s\n", entry->amiga.comment);
    }
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
    int force_udf = 0, force_hfs = 0;
    imgls_opts_t ls_opts;

    memset(&ls_opts, 0, sizeof(ls_opts));

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-u") == 0 || strcmp(argv[i], "--udf") == 0)
            force_udf = 1;
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--hfs") == 0)
            force_hfs = 1;
        else if (strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "--amiga") == 0)
            ls_opts.show_amiga = 1;
        else if (!image)
            image = argv[i];
        else
            path = argv[i];
    }

    if (!image) {
        fprintf(stderr, "usage: imgls [-u|-h] [-a] <image> [path]\n");
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
    if (force_hfs)
        opts.prefer_hfs = 1;

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

    err = odfs_readdir(&mnt, &dir, print_entry, &ls_opts, NULL);
    if (err != ODFS_OK) {
        fprintf(stderr, "error: readdir: %s\n", odfs_err_str(err));
        odfs_unmount(&mnt);
        return 1;
    }

    odfs_unmount(&mnt);
    return 0;
}
