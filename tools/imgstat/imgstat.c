/*
 * imgstat — print metadata and backend details
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "odfs/api.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    odfs_media_t media;
    odfs_mount_t mnt;
    odfs_mount_opts_t opts;
    odfs_log_state_t log;
    odfs_err_t err;

    if (argc < 3) {
        fprintf(stderr, "usage: imgstat <image.iso> <path>\n");
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

    printf("path:    %s\n", argv[2]);
    printf("name:    %s\n", node.name);
    printf("kind:    %s\n", odfs_node_kind_name(node.kind));
    printf("backend: %s\n", odfs_backend_type_name(node.backend));
    printf("size:    %llu\n", (unsigned long long)node.size);
    printf("id:      %u\n", node.id);
    printf("parent:  %u\n", node.parent_id);
    printf("extent:  LBA %u, %u bytes\n", node.extent.lba, node.extent.length);
    if (node.mode)
        printf("mode:    %04o\n", node.mode);
    printf("mtime:   %04d-%02u-%02u %02u:%02u:%02u\n",
           node.mtime.year, node.mtime.month, node.mtime.day,
           node.mtime.hour, node.mtime.minute, node.mtime.second);

    odfs_unmount(&mnt);
    return 0;
}
