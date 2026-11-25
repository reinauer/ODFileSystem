/*
 * test_namefix.c — tests for deterministic duplicate-name handling
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "odfs/charset.h"
#include "odfs/namefix.h"
#include "odfs/node.h"
#include "test_harness.h"

static const odfs_names_t names_utf8 = { ODFS_NAMES_UTF8, NULL };
static const odfs_names_t names_latin1 = { ODFS_NAMES_LATIN1, NULL };

TEST(namefix_keeps_unique_name)
{
    odfs_namefix_state_t state;
    char name[ODFS_NAME_MAX] = "README";

    odfs_namefix_init(&state, &names_utf8);
    ASSERT_OK(odfs_namefix_apply(&state, name, sizeof(name)));
    ASSERT_STR_EQ(name, "README");
    odfs_namefix_destroy(&state);
}

TEST(namefix_suffixes_duplicate_name)
{
    odfs_namefix_state_t state;
    char first[ODFS_NAME_MAX] = "README";
    char second[ODFS_NAME_MAX] = "README";

    odfs_namefix_init(&state, &names_utf8);
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

    odfs_namefix_init(&state, &names_utf8);
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

    odfs_namefix_init(&state, &names_utf8);
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

        odfs_namefix_init(&state, &names_utf8);
        ASSERT_OK(odfs_namefix_apply(&state, a, sizes[i]));
        ASSERT_OK(odfs_namefix_apply(&state, b, sizes[i]));
        ASSERT_STR_EQ(b, want[i]);
        odfs_namefix_destroy(&state);
    }
}

TEST(namefix_converts_names_to_latin1)
{
    odfs_namefix_state_t state;
    /* "Gr" U+00FC U+00DF "e.txt" */
    char name[ODFS_NAME_MAX] = "Gr\xC3\xBC\xC3\x9F" "e.txt";
    char ascii[ODFS_NAME_MAX] = "README";

    odfs_namefix_init(&state, &names_latin1);
    ASSERT_OK(odfs_namefix_apply(&state, name, sizeof(name)));
    ASSERT_STR_EQ(name, "Gr\xFC\xDF" "e.txt");
    ASSERT_OK(odfs_namefix_apply(&state, ascii, sizeof(ascii)));
    ASSERT_STR_EQ(ascii, "README");
    odfs_namefix_destroy(&state);
}

TEST(namefix_keeps_utf8_names_by_default)
{
    odfs_namefix_state_t state;
    char name[ODFS_NAME_MAX] = "Gr\xC3\xBC\xC3\x9F" "e.txt";

    odfs_namefix_init(&state, &names_utf8);
    ASSERT_OK(odfs_namefix_apply(&state, name, sizeof(name)));
    ASSERT_STR_EQ(name, "Gr\xC3\xBC\xC3\x9F" "e.txt");
    odfs_namefix_destroy(&state);
}

/* names that differ in UTF-8 but come out the same must not collide */
TEST(namefix_separates_names_that_convert_alike)
{
    odfs_namefix_state_t state;
    /* U+65E5 U+672C ".txt", U+4E2D U+56FD ".txt", and a literal "__.txt" */
    char a[ODFS_NAME_MAX] = "\xE6\x97\xA5\xE6\x9C\xAC.txt";
    char b[ODFS_NAME_MAX] = "\xE4\xB8\xAD\xE5\x9B\xBD.txt";
    char c[ODFS_NAME_MAX] = "__.txt";
    /* the same name once as UTF-8 and once as raw Latin-1 */
    char d[ODFS_NAME_MAX] = "caf\xC3\xA9";
    char e[ODFS_NAME_MAX] = "caf\xE9";

    odfs_namefix_init(&state, &names_latin1);
    ASSERT_OK(odfs_namefix_apply(&state, a, sizeof(a)));
    ASSERT_OK(odfs_namefix_apply(&state, b, sizeof(b)));
    ASSERT_OK(odfs_namefix_apply(&state, c, sizeof(c)));
    ASSERT_STR_EQ(a, "__.txt");
    ASSERT_STR_EQ(b, "__.txt~2");
    ASSERT_STR_EQ(c, "__.txt~3");
    ASSERT_OK(odfs_namefix_apply(&state, d, sizeof(d)));
    ASSERT_OK(odfs_namefix_apply(&state, e, sizeof(e)));
    ASSERT_STR_EQ(d, "caf\xE9");
    ASSERT_STR_EQ(e, "caf\xE9~2");
    odfs_namefix_destroy(&state);
}

/* in Latin-1, 0x80-0xBF are characters, not the tail of one */
TEST(namefix_truncates_latin1_names_at_any_byte)
{
    odfs_namefix_state_t state;
    char a[8] = "ab\xA9";
    char b[8] = "ab\xA9";

    odfs_namefix_init(&state, &names_latin1);
    ASSERT_OK(odfs_namefix_apply(&state, a, 5));
    ASSERT_OK(odfs_namefix_apply(&state, b, 5));
    ASSERT_STR_EQ(a, "ab\xA9");
    ASSERT_STR_EQ(b, "ab~2");
    odfs_namefix_destroy(&state);
}

/* a mount can name its own 8-bit set */
TEST(namefix_converts_names_through_a_map)
{
#if ODFS_FEATURE_CHARSET_EXTERNAL
    /* U+0142 at 0xB3 and U+00F3 at 0xF3, as in ISO 8859-2 */
    static uint16_t map[ODFS_CHARSET_MAP_SIZE];
    const odfs_names_t names = { ODFS_NAMES_MAPPED, map };
    odfs_namefix_state_t state;
    /* "L" U+00F3 "d" U+017A ".txt" and "l" U+00F3 "d_.txt" */
    char a[ODFS_NAME_MAX] = "\xC5\x81\xC3\xB3" "d\xC5\xBA.txt";
    char b[ODFS_NAME_MAX] = "\xC5\x82\xC3\xB3" "d_.txt";

    map[0xA3 - ODFS_CHARSET_MAP_FIRST] = 0x0141;
    map[0xB3 - ODFS_CHARSET_MAP_FIRST] = 0x0142;
    map[0xF3 - ODFS_CHARSET_MAP_FIRST] = 0x00F3;

    odfs_namefix_init(&state, &names);
    ASSERT_OK(odfs_namefix_apply(&state, a, sizeof(a)));
    ASSERT_STR_EQ(a, "\xA3\xF3" "d_.txt");
    /* the same name but for the case of its first letter */
    ASSERT_OK(odfs_namefix_apply(&state, b, sizeof(b)));
    ASSERT_STR_EQ(b, "\xB3\xF3" "d_.txt");
    odfs_namefix_destroy(&state);
#endif
}

TEST_MAIN()
