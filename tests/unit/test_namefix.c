/*
 * test_namefix.c — tests for deterministic duplicate-name handling
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "odfs/namefix.h"
#include "odfs/node.h"
#include "test_harness.h"

TEST(namefix_keeps_unique_name)
{
    odfs_namefix_state_t state;
    char name[ODFS_NAME_MAX] = "README";

    odfs_namefix_init(&state);
    ASSERT_OK(odfs_namefix_apply(&state, name, sizeof(name)));
    ASSERT_STR_EQ(name, "README");
    odfs_namefix_destroy(&state);
}

TEST(namefix_suffixes_duplicate_name)
{
    odfs_namefix_state_t state;
    char first[ODFS_NAME_MAX] = "README";
    char second[ODFS_NAME_MAX] = "README";

    odfs_namefix_init(&state);
    ASSERT_OK(odfs_namefix_apply(&state, first, sizeof(first)));
    ASSERT_OK(odfs_namefix_apply(&state, second, sizeof(second)));
    ASSERT_STR_EQ(first, "README");
    ASSERT_STR_EQ(second, "README~2");
    odfs_namefix_destroy(&state);
}

TEST(namefix_handles_suffix_collisions)
{
    odfs_namefix_state_t state;
    char first[ODFS_NAME_MAX] = "README";
    char second[ODFS_NAME_MAX] = "README";
    char third[ODFS_NAME_MAX] = "README~2";

    odfs_namefix_init(&state);
    ASSERT_OK(odfs_namefix_apply(&state, first, sizeof(first)));
    ASSERT_OK(odfs_namefix_apply(&state, second, sizeof(second)));
    ASSERT_OK(odfs_namefix_apply(&state, third, sizeof(third)));
    ASSERT_STR_EQ(second, "README~2");
    ASSERT_STR_EQ(third, "README~2~2");
    odfs_namefix_destroy(&state);
}

TEST(namefix_is_case_insensitive)
{
    odfs_namefix_state_t state;
    char first[ODFS_NAME_MAX] = "ReadMe";
    char second[ODFS_NAME_MAX] = "README";

    odfs_namefix_init(&state);
    ASSERT_OK(odfs_namefix_apply(&state, first, sizeof(first)));
    ASSERT_OK(odfs_namefix_apply(&state, second, sizeof(second)));
    ASSERT_STR_EQ(second, "README~2");
    odfs_namefix_destroy(&state);
}

TEST_MAIN()
