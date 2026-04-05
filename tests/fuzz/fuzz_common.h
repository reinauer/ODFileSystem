/*
 * fuzz_common.h — shared smoke harness for parser fuzz targets
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef ODFS_FUZZ_COMMON_H
#define ODFS_FUZZ_COMMON_H

#include "odfs/api.h"
#include <stdio.h>
#include <string.h>

typedef struct fuzz_scan_result {
    odfs_node_t first_file;
    odfs_node_t first_dir;
    int         have_file;
    int         have_dir;
} fuzz_scan_result_t;

static odfs_err_t fuzz_collect_entry(const odfs_node_t *entry, void *ctx)
{
    fuzz_scan_result_t *scan = ctx;

    if (!scan->have_file && entry->kind == ODFS_NODE_FILE) {
        scan->first_file = *entry;
        scan->have_file = 1;
    }
    if (!scan->have_dir && entry->kind == ODFS_NODE_DIR) {
        scan->first_dir = *entry;
        scan->have_dir = 1;
    }

    return ODFS_OK;
}

static void fuzz_scan_dir(odfs_mount_t *mnt, const odfs_node_t *dir,
                          fuzz_scan_result_t *scan)
{
    memset(scan, 0, sizeof(*scan));
    (void)odfs_readdir(mnt, dir, fuzz_collect_entry, scan, NULL);
}

static void fuzz_try_lookup(odfs_mount_t *mnt, const odfs_node_t *dir,
                            const char *name)
{
    odfs_node_t tmp;
    char path[ODFS_NAME_MAX + 2];

    (void)odfs_lookup(mnt, dir, name, &tmp);

    path[0] = '/';
    strncpy(&path[1], name, ODFS_NAME_MAX);
    path[ODFS_NAME_MAX + 1] = '\0';
    (void)odfs_resolve_path(mnt, path, &tmp);
}

static void fuzz_try_read(odfs_mount_t *mnt, const odfs_node_t *file)
{
    uint8_t buf[4096];
    size_t len;

    if (file->size == 0)
        return;

    len = sizeof(buf);
    if (file->size < len)
        len = (size_t)file->size;

    (void)odfs_read(mnt, file, 0, buf, &len);
}

static int fuzz_one_path(const char *path, odfs_backend_type_t force_backend)
{
    odfs_media_t media;
    odfs_mount_t mnt;
    odfs_mount_opts_t opts;
    odfs_log_state_t log;
    odfs_err_t err;
    fuzz_scan_result_t root_scan;
    fuzz_scan_result_t subdir_scan;

    err = odfs_media_open_image(path, &media);
    if (err != ODFS_OK) {
        fprintf(stderr, "fuzz: cannot open '%s': %s\n", path, odfs_err_str(err));
        return 1;
    }

    odfs_log_init(&log);
    odfs_mount_opts_default(&opts);
    opts.force_backend = force_backend;

    err = odfs_mount(&media, &opts, &log, &mnt);
    if (err != ODFS_OK) {
        odfs_media_close(&media);
        return 0;
    }

    fuzz_scan_dir(&mnt, &mnt.root, &root_scan);

    if (root_scan.have_file) {
        fuzz_try_lookup(&mnt, &mnt.root, root_scan.first_file.name);
        fuzz_try_read(&mnt, &root_scan.first_file);
    }

    if (root_scan.have_dir) {
        fuzz_try_lookup(&mnt, &mnt.root, root_scan.first_dir.name);
        fuzz_scan_dir(&mnt, &root_scan.first_dir, &subdir_scan);
        if (subdir_scan.have_file)
            fuzz_try_read(&mnt, &subdir_scan.first_file);
    }

    odfs_unmount(&mnt);
    return 0;
}

static int fuzz_main_paths(int argc, char **argv,
                           odfs_backend_type_t force_backend,
                           const char *name)
{
    int failed = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <image> [image ...]\n", name);
        return 2;
    }

    for (int i = 1; i < argc; i++) {
        if (fuzz_one_path(argv[i], force_backend) != 0)
            failed = 1;
    }

    return failed;
}

#endif /* ODFS_FUZZ_COMMON_H */
