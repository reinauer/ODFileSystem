/*
 * test_resolve_parent.c — backend resolve_parent fast-path coverage
 *
 * Mounts real filesystem fixtures and verifies that
 * odfs_resolve_parent_node returns the correct parent and grandparent, and
 * that its result is identical to the generic tree search fallback.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "odfs/api.h"
#include "odfs/ancestry.h"
#include "odfs/error.h"
#include "test_harness.h"

#include <string.h>

/* these fixtures contain a two-level DEEP/SUBDIR-style directory tree */
#define IMG_PLAIN   "tests/images/test_plain.iso"
#define IMG_JOLIET  "tests/images/test_joliet.iso"
#define IMG_UDF     "tests/images/test_udf_bridge.iso"
#define IMG_HFS     "tests/images/test_hfs.iso"
#define IMG_HFSPLUS "tests/images/test_hfsplus.iso"

static int mount_image(const char *path, odfs_backend_type_t force_backend,
                       int prefer_hfs,
                       odfs_media_t *media, odfs_log_state_t *log,
                       odfs_mount_t *mnt)
{
    odfs_mount_opts_t opts;

    if (odfs_media_open_image(path, media) != ODFS_OK)
        return 0;

    odfs_log_init(log);
    odfs_mount_opts_default(&opts);
    opts.force_backend = force_backend;
    opts.prefer_hfs = prefer_hfs;

    if (odfs_mount(media, &opts, log, mnt) != ODFS_OK) {
        odfs_media_close(media);
        return 0;
    }
    return 1;
}

/*
 * Resolve via the backend fast path, then via the generic search, and assert
 * the two agree on identity (extent + name). Writes the fast-path parent and
 * grandparent into (parent_out, grandparent_out) for further inspection.
 *
 * A macro rather than a function so the ASSERT_* checks abort the enclosing
 * TEST (they reference the TEST-local _fail_count).
 */
#define ASSERT_PARENT_MATCHES_SEARCH(mnt, node, parent_out, grandparent_out) \
    do { \
        odfs_node_t _sp, _sgp; \
        ASSERT_OK(odfs_resolve_parent_node((mnt), (node), \
                                           (parent_out), (grandparent_out))); \
        ASSERT_OK(odfs_resolve_parent_search((mnt), (node), &_sp, &_sgp)); \
        ASSERT(odfs_node_matches_identity((parent_out), &_sp)); \
        ASSERT(odfs_node_matches_identity((grandparent_out), &_sgp)); \
    } while (0)

TEST(resolve_parent_iso_two_levels_deep)
{
    odfs_media_t media;
    odfs_log_state_t log;
    odfs_mount_t mnt;
    odfs_node_t subdir, parent, grandparent;

    if (!mount_image(IMG_PLAIN, ODFS_BACKEND_ISO9660, 0,
                     &media, &log, &mnt)) {
        printf("  (skipped: %s)\n", "fixture " IMG_PLAIN " unavailable");
        return;
    }

    /* /DEEP/SUBDIR -> parent DEEP, grandparent root */
    ASSERT_OK(odfs_resolve_path(&mnt, "/DEEP/SUBDIR", &subdir));
    ASSERT_PARENT_MATCHES_SEARCH(&mnt, &subdir, &parent, &grandparent);
    ASSERT_STR_EQ(parent.name, "DEEP");
    ASSERT(odfs_node_matches_identity(&grandparent, &mnt.root));

    odfs_unmount(&mnt);
}

TEST(resolve_parent_iso_top_level)
{
    odfs_media_t media;
    odfs_log_state_t log;
    odfs_mount_t mnt;
    odfs_node_t deep, parent, grandparent;

    if (!mount_image(IMG_PLAIN, ODFS_BACKEND_ISO9660, 0,
                     &media, &log, &mnt)) {
        printf("  (skipped: %s)\n", "fixture " IMG_PLAIN " unavailable");
        return;
    }

    /* /DEEP -> parent root, grandparent root */
    ASSERT_OK(odfs_resolve_path(&mnt, "/DEEP", &deep));
    ASSERT_PARENT_MATCHES_SEARCH(&mnt, &deep, &parent, &grandparent);
    ASSERT(odfs_node_matches_identity(&parent, &mnt.root));
    ASSERT(odfs_node_matches_identity(&grandparent, &mnt.root));

    odfs_unmount(&mnt);
}

TEST(resolve_parent_root_has_no_parent)
{
    odfs_media_t media;
    odfs_log_state_t log;
    odfs_mount_t mnt;
    odfs_node_t parent;

    if (!mount_image(IMG_PLAIN, ODFS_BACKEND_ISO9660, 0,
                     &media, &log, &mnt)) {
        printf("  (skipped: %s)\n", "fixture " IMG_PLAIN " unavailable");
        return;
    }

    ASSERT_ERR(odfs_resolve_parent_node(&mnt, &mnt.root, &parent, NULL),
               ODFS_ERR_NOT_FOUND);

    odfs_unmount(&mnt);
}

TEST(resolve_parent_without_grandparent_out)
{
    odfs_media_t media;
    odfs_log_state_t log;
    odfs_mount_t mnt;
    odfs_node_t subdir, parent;

    if (!mount_image(IMG_PLAIN, ODFS_BACKEND_ISO9660, 0,
                     &media, &log, &mnt)) {
        printf("  (skipped: %s)\n", "fixture " IMG_PLAIN " unavailable");
        return;
    }

    /* grandparent_out == NULL must be accepted */
    ASSERT_OK(odfs_resolve_path(&mnt, "/DEEP/SUBDIR", &subdir));
    ASSERT_OK(odfs_resolve_parent_node(&mnt, &subdir, &parent, NULL));
    ASSERT_STR_EQ(parent.name, "DEEP");

    odfs_unmount(&mnt);
}

TEST(resolve_parent_joliet)
{
    odfs_media_t media;
    odfs_log_state_t log;
    odfs_mount_t mnt;
    odfs_node_t dir, parent, grandparent;

    if (!mount_image(IMG_JOLIET, ODFS_BACKEND_JOLIET, 0,
                     &media, &log, &mnt)) {
        printf("  (skipped: %s)\n", "fixture " IMG_JOLIET " unavailable");
        return;
    }

    /* the Joliet fixture mirrors the plain ISO: /DEEP/SUBDIR */
    if (odfs_resolve_path(&mnt, "/DEEP/SUBDIR", &dir) == ODFS_OK) {
        odfs_node_t level1;

        ASSERT_PARENT_MATCHES_SEARCH(&mnt, &dir, &parent, &grandparent);
        ASSERT(odfs_node_matches_identity(&grandparent, &mnt.root));

        /* and the parent (DEEP) resolves up to the root */
        ASSERT_PARENT_MATCHES_SEARCH(&mnt, &parent, &level1, &grandparent);
        ASSERT(odfs_node_matches_identity(&level1, &mnt.root));
    } else {
        printf("  (skipped: %s)\n", "Joliet fixture lacks /DEEP/SUBDIR");
    }

    odfs_unmount(&mnt);
}

TEST(resolve_parent_udf)
{
    odfs_media_t media;
    odfs_log_state_t log;
    odfs_mount_t mnt;
    odfs_node_t dir, parent, grandparent;

    if (!mount_image(IMG_UDF, ODFS_BACKEND_UDF, 0, &media, &log, &mnt)) {
        printf("  (skipped: %s)\n", "fixture " IMG_UDF " unavailable");
        return;
    }

    ASSERT_OK(odfs_resolve_path(&mnt, "/deep/subdir", &dir));
    ASSERT_PARENT_MATCHES_SEARCH(&mnt, &dir, &parent, &grandparent);
    ASSERT_STR_EQ(parent.name, "deep");
    ASSERT(odfs_node_matches_identity(&grandparent, &mnt.root));

    odfs_unmount(&mnt);
}

TEST(resolve_parent_hfs)
{
    odfs_media_t media;
    odfs_log_state_t log;
    odfs_mount_t mnt;
    odfs_node_t dir, parent, grandparent;

    if (!mount_image(IMG_HFS, ODFS_BACKEND_NONE, 1, &media, &log, &mnt)) {
        printf("  (skipped: %s)\n", "fixture " IMG_HFS " unavailable");
        return;
    }

    ASSERT_OK(odfs_resolve_path(&mnt, "/DEEP/SUBDIR", &dir));
    ASSERT_PARENT_MATCHES_SEARCH(&mnt, &dir, &parent, &grandparent);
    ASSERT_STR_EQ(parent.name, "DEEP");
    ASSERT(odfs_node_matches_identity(&grandparent, &mnt.root));

    odfs_unmount(&mnt);
}

TEST(resolve_parent_hfsplus)
{
    odfs_media_t media;
    odfs_log_state_t log;
    odfs_mount_t mnt;
    odfs_node_t dir, parent, grandparent;

    if (!mount_image(IMG_HFSPLUS, ODFS_BACKEND_HFSPLUS, 0,
                     &media, &log, &mnt)) {
        printf("  (skipped: %s)\n", "fixture " IMG_HFSPLUS " unavailable");
        return;
    }

    ASSERT_OK(odfs_resolve_path(&mnt, "/deep/subdir", &dir));
    ASSERT_PARENT_MATCHES_SEARCH(&mnt, &dir, &parent, &grandparent);
    ASSERT_STR_EQ(parent.name, "deep");
    ASSERT(odfs_node_matches_identity(&grandparent, &mnt.root));

    odfs_unmount(&mnt);
}

TEST_MAIN()
