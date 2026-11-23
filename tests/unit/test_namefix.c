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

/* a duplicate name shortened for its suffix keeps whole characters */
TEST(namefix_truncates_at_utf8_boundaries)
{
    /* "ab" U+00E9, "a" U+4E16, "x" U+1F600: each ends in a multi-byte
     * character that would straddle the cut before "~2" */
    static const char *const names[3] = {
        "ab\xC3\xA9", "a\xE4\xB8\x96", "x\xF0\x9F\x98\x80"
    };
    static const size_t sizes[3] = { 6, 6, 7 };
    static const char *const want[3] = { "ab~2", "a~2", "x~2" };
    unsigned int i;

    for (i = 0; i < 3; i++) {
        odfs_namefix_state_t state;
        char a[8];
        char b[8];

        memset(a, 0, sizeof(a));
        strcpy(a, names[i]);
        memcpy(b, a, sizeof(b));

        odfs_namefix_init(&state);
        ASSERT_OK(odfs_namefix_apply(&state, a, sizes[i]));
        ASSERT_OK(odfs_namefix_apply(&state, b, sizes[i]));
        ASSERT_STR_EQ(b, want[i]);
        odfs_namefix_destroy(&state);
    }
}

TEST_MAIN()
