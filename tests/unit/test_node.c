/*
 * test_node.c — tests for node model
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "odfs/node.h"
#include "test_harness.h"

TEST(backend_type_names)
{
    ASSERT_STR_EQ(odfs_backend_type_name(ODFS_BACKEND_NONE), "none");
    ASSERT_STR_EQ(odfs_backend_type_name(ODFS_BACKEND_ISO9660), "iso9660");
    ASSERT_STR_EQ(odfs_backend_type_name(ODFS_BACKEND_ROCK_RIDGE), "rock_ridge");
    ASSERT_STR_EQ(odfs_backend_type_name(ODFS_BACKEND_JOLIET), "joliet");
    ASSERT_STR_EQ(odfs_backend_type_name(ODFS_BACKEND_UDF), "udf");
    ASSERT_STR_EQ(odfs_backend_type_name(ODFS_BACKEND_HFS), "hfs");
    ASSERT_STR_EQ(odfs_backend_type_name(ODFS_BACKEND_HFSPLUS), "hfsplus");
    ASSERT_STR_EQ(odfs_backend_type_name(ODFS_BACKEND_CDDA), "cdda");
}

TEST(backend_type_unknown)
{
    ASSERT_STR_EQ(odfs_backend_type_name((odfs_backend_type_t)99), "unknown");
}

TEST(node_kind_names)
{
    ASSERT_STR_EQ(odfs_node_kind_name(ODFS_NODE_FILE), "file");
    ASSERT_STR_EQ(odfs_node_kind_name(ODFS_NODE_DIR), "dir");
    ASSERT_STR_EQ(odfs_node_kind_name(ODFS_NODE_SYMLINK), "symlink");
    ASSERT_STR_EQ(odfs_node_kind_name(ODFS_NODE_VIRTUAL), "virtual");
}

TEST(node_kind_unknown)
{
    ASSERT_STR_EQ(odfs_node_kind_name((odfs_node_kind_t)99), "unknown");
}

TEST_MAIN()
