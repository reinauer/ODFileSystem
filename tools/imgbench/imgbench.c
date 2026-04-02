/*
 * imgbench — cache and read benchmark/stats
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

    if (argc < 2) {
        fprintf(stderr, "usage: imgbench <image.iso>\n");
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

    /* print cache stats after mount */
    const odfs_cache_stats_t *stats = odfs_cache_get_stats(&mnt.cache);
    printf("cache stats after mount:\n");
    printf("  reads:     %u\n", stats->reads);
    printf("  hits:      %u\n", stats->hits);
    printf("  misses:    %u\n", stats->misses);
    printf("  evictions: %u\n", stats->evictions);
    printf("  max used:  %u\n", stats->max_used);

    odfs_unmount(&mnt);
    return 0;
}
