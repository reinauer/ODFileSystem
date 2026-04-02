/*
 * imginfo — detect and summarize media/image
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "odfs/api.h"
#include <stdio.h>
#include <stdlib.h>

static void stderr_sink(odfs_log_level_t level, odfs_log_subsys_t subsys,
                        const char *msg, void *ctx)
{
    (void)level; (void)subsys; (void)ctx;
    fprintf(stderr, "%s\n", msg);
}

int main(int argc, char **argv)
{
    odfs_media_t media;
    odfs_mount_t mnt;
    odfs_mount_opts_t opts;
    odfs_log_state_t log;
    odfs_err_t err;

    if (argc < 2) {
        fprintf(stderr, "usage: imginfo <image.iso>\n");
        return 1;
    }

    err = odfs_media_open_image(argv[1], &media);
    if (err != ODFS_OK) {
        fprintf(stderr, "error: cannot open '%s': %s\n",
                argv[1], odfs_err_str(err));
        return 1;
    }

    printf("image: %s\n", argv[1]);
    printf("sector size: %u\n", odfs_media_sector_size(&media));
    printf("sector count: %u\n", odfs_media_sector_count(&media));
    printf("total size: %llu bytes\n",
           (unsigned long long)odfs_media_sector_size(&media) *
           odfs_media_sector_count(&media));

    /* try to mount and report detected formats */
    odfs_log_init(&log);
    odfs_log_set_sink(&log, stderr_sink, NULL);
    odfs_log_set_level(&log, ODFS_LOG_INFO);
    odfs_mount_opts_default(&opts);

    err = odfs_mount(&media, &opts, &log, &mnt);
    if (err == ODFS_OK) {
        printf("backend: %s\n", odfs_backend_type_name(mnt.active_backend));
        printf("volume: %s\n", mnt.volume_name);
        odfs_unmount(&mnt);
    } else {
        printf("mount: %s\n", odfs_err_str(err));
        odfs_media_close(&media);
    }

    return 0;
}
