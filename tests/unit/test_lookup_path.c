/*
 * test_lookup_path.c — multi-name lookups served from the metadata cache
 *
 * odfs_lookup_path() must agree with looking the names up one by one,
 * and must leave every case it does not handle (uncached directories,
 * empty names, symbolic links) to the caller.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "odfs/api.h"
#include "odfs/error.h"
#include "test_harness.h"

#include <string.h>

/* Rock Ridge fixture: deep/subdir/nested.txt and a root symlink.txt */
#define IMG_RR "tests/images/test_rr.iso"

static int mount_image(odfs_media_t *media, odfs_log_state_t *log,
                       odfs_mount_t *mnt)
{
    odfs_mount_opts_t opts;

    if (odfs_media_open_image(IMG_RR, media) != ODFS_OK)
        return 0;

    odfs_log_init(log);
    odfs_mount_opts_default(&opts);
    if (odfs_mount(media, &opts, log, mnt) != ODFS_OK) {
        odfs_media_close(media);
        return 0;
    }
    return 1;
}

#define ASSERT_SAME_NODE(a, b) \
    do { \
        ASSERT(odfs_node_matches_identity((a), (b))); \
        ASSERT_STR_EQ((a)->name, (b)->name); \
        ASSERT_EQ((a)->id, (b)->id); \
        ASSERT_EQ((a)->parent_id, (b)->parent_id); \
        ASSERT_EQ((a)->kind, (b)->kind); \
        ASSERT_EQ((a)->size, (b)->size); \
    } while (0)

/* look the names up one by one, which also caches the directories */
#define LOOKUP_CHAIN(mnt, deep, subdir, nested) \
    do { \
        ASSERT_OK(odfs_lookup((mnt), &(mnt)->root, "deep", (deep))); \
        ASSERT_OK(odfs_lookup((mnt), (deep), "subdir", (subdir))); \
        ASSERT_OK(odfs_lookup((mnt), (subdir), "nested.txt", (nested))); \
    } while (0)

TEST(lookup_path_matches_name_by_name)
{
    odfs_media_t media;
    odfs_log_state_t log;
    odfs_mount_t mnt;
    odfs_node_t deep, subdir, nested;
    odfs_node_t out, parent, grandparent;
    unsigned int depth = 0;

    if (!mount_image(&media, &log, &mnt)) {
        printf("  (skipped: fixture " IMG_RR " unavailable)\n");
        return;
    }

    LOOKUP_CHAIN(&mnt, &deep, &subdir, &nested);

    ASSERT_OK(odfs_lookup_path(&mnt, &mnt.root, "deep/subdir/nested.txt",
                               &out, &parent, &grandparent, &depth));
    ASSERT_EQ(depth, 3);
    ASSERT_SAME_NODE(&out, &nested);
    ASSERT_SAME_NODE(&parent, &subdir);
    ASSERT_SAME_NODE(&grandparent, &deep);

    /* names compare without regard to case, as single lookups do */
    ASSERT_OK(odfs_lookup_path(&mnt, &mnt.root, "DEEP/SubDir/NESTED.TXT",
                               &out, &parent, &grandparent, &depth));
    ASSERT_EQ(depth, 3);
    ASSERT_SAME_NODE(&out, &nested);

    /* two names: no grandparent below the starting directory */
    ASSERT_OK(odfs_lookup_path(&mnt, &deep, "subdir/nested.txt",
                               &out, &parent, &grandparent, &depth));
    ASSERT_EQ(depth, 2);
    ASSERT_SAME_NODE(&out, &nested);
    ASSERT_SAME_NODE(&parent, &subdir);

    /* a single trailing '/' is only a separator */
    ASSERT_OK(odfs_lookup_path(&mnt, &mnt.root, "deep/subdir/",
                               &out, &parent, &grandparent, &depth));
    ASSERT_EQ(depth, 2);
    ASSERT_SAME_NODE(&out, &subdir);
    ASSERT_SAME_NODE(&parent, &deep);

    odfs_unmount(&mnt);
}

TEST(lookup_path_needs_cached_directories)
{
    odfs_media_t media;
    odfs_log_state_t log;
    odfs_mount_t mnt;
    odfs_node_t deep, subdir, nested;
    odfs_node_t out, parent, grandparent;
    unsigned int depth = 0;

    if (!mount_image(&media, &log, &mnt)) {
        printf("  (skipped: fixture " IMG_RR " unavailable)\n");
        return;
    }

    /* nothing below the root is cached yet */
    ASSERT_OK(odfs_lookup(&mnt, &mnt.root, "deep", &deep));
    ASSERT_EQ(odfs_lookup_path(&mnt, &mnt.root, "deep/subdir/nested.txt",
                               &out, &parent, &grandparent, &depth),
              ODFS_ERR_UNSUPPORTED);

    LOOKUP_CHAIN(&mnt, &deep, &subdir, &nested);
    ASSERT_OK(odfs_lookup_path(&mnt, &mnt.root, "deep/subdir/nested.txt",
                               &out, &parent, &grandparent, &depth));

    odfs_unmount(&mnt);
}

TEST(lookup_path_reports_missing_names_and_files)
{
    odfs_media_t media;
    odfs_log_state_t log;
    odfs_mount_t mnt;
    odfs_node_t deep, subdir, nested;
    odfs_node_t out, parent, grandparent;
    unsigned int depth = 0;

    if (!mount_image(&media, &log, &mnt)) {
        printf("  (skipped: fixture " IMG_RR " unavailable)\n");
        return;
    }

    LOOKUP_CHAIN(&mnt, &deep, &subdir, &nested);

    ASSERT_EQ(odfs_lookup_path(&mnt, &mnt.root, "deep/subdir/none.txt",
                               &out, &parent, &grandparent, &depth),
              ODFS_ERR_NOT_FOUND);
    ASSERT_EQ(odfs_lookup_path(&mnt, &mnt.root, "deep/none/nested.txt",
                               &out, &parent, &grandparent, &depth),
              ODFS_ERR_NOT_FOUND);
    ASSERT_EQ(odfs_lookup_path(&mnt, &mnt.root, "SHORT.TXT/x",
                               &out, &parent, &grandparent, &depth),
              ODFS_ERR_NOT_DIR);

    odfs_unmount(&mnt);
}

TEST(lookup_path_leaves_climbs_and_links_to_caller)
{
    odfs_media_t media;
    odfs_log_state_t log;
    odfs_mount_t mnt;
    odfs_node_t deep, subdir, nested;
    odfs_node_t out, parent, grandparent;
    unsigned int depth = 0;

    if (!mount_image(&media, &log, &mnt)) {
        printf("  (skipped: fixture " IMG_RR " unavailable)\n");
        return;
    }

    LOOKUP_CHAIN(&mnt, &deep, &subdir, &nested);

    /* an empty name climbs to the parent directory */
    ASSERT_EQ(odfs_lookup_path(&mnt, &mnt.root, "deep//deep",
                               &out, &parent, &grandparent, &depth),
              ODFS_ERR_UNSUPPORTED);
    ASSERT_EQ(odfs_lookup_path(&mnt, &deep, "/deep",
                               &out, &parent, &grandparent, &depth),
              ODFS_ERR_UNSUPPORTED);
    /* "SHORT.TXT//x" climbs out of the file before it would fail */
    ASSERT_EQ(odfs_lookup_path(&mnt, &mnt.root, "SHORT.TXT//x",
                               &out, &parent, &grandparent, &depth),
              ODFS_ERR_UNSUPPORTED);
    /* links are resolved by the caller */
    ASSERT_EQ(odfs_lookup_path(&mnt, &mnt.root, "symlink.txt/x",
                               &out, &parent, &grandparent, &depth),
              ODFS_ERR_UNSUPPORTED);
    /* a walk must start in a directory */
    ASSERT_EQ(odfs_lookup_path(&mnt, &nested, "x/y",
                               &out, &parent, &grandparent, &depth),
              ODFS_ERR_UNSUPPORTED);

    odfs_unmount(&mnt);
}

TEST_MAIN()
