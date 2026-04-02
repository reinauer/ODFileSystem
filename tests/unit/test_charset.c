/*
 * test_charset.c — tests for character conversion
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "odfs/charset.h"
#include "test_harness.h"

TEST(ucs2be_ascii)
{
    /* "HELLO" in UCS-2BE */
    const uint8_t src[] = { 0x00,'H', 0x00,'E', 0x00,'L', 0x00,'L', 0x00,'O' };
    char dst[64];
    size_t len;

    ASSERT_OK(odfs_ucs2be_to_utf8(src, sizeof(src), dst, sizeof(dst), &len));
    ASSERT_STR_EQ(dst, "HELLO");
    ASSERT_EQ(len, 5);
}

TEST(ucs2be_accented)
{
    /* U+00E9 = é (2-byte UTF-8: C3 A9) */
    const uint8_t src[] = { 0x00,0xE9 };
    char dst[64];
    size_t len;

    ASSERT_OK(odfs_ucs2be_to_utf8(src, sizeof(src), dst, sizeof(dst), &len));
    ASSERT_EQ(len, 2);
    ASSERT_EQ((unsigned char)dst[0], 0xC3);
    ASSERT_EQ((unsigned char)dst[1], 0xA9);
    ASSERT_EQ(dst[2], '\0');
}

TEST(ucs2be_cjk)
{
    /* U+4E16 = 世 (3-byte UTF-8: E4 B8 96) */
    const uint8_t src[] = { 0x4E, 0x16 };
    char dst[64];
    size_t len;

    ASSERT_OK(odfs_ucs2be_to_utf8(src, sizeof(src), dst, sizeof(dst), &len));
    ASSERT_EQ(len, 3);
    ASSERT_EQ((unsigned char)dst[0], 0xE4);
    ASSERT_EQ((unsigned char)dst[1], 0xB8);
    ASSERT_EQ((unsigned char)dst[2], 0x96);
}

TEST(ucs2be_odd_length)
{
    const uint8_t src[] = { 0x00, 'A', 0x00 }; /* odd byte count */
    char dst[64];
    size_t len;

    ASSERT_ERR(odfs_ucs2be_to_utf8(src, 3, dst, sizeof(dst), &len),
               ODFS_ERR_INVAL);
}

TEST(iso_name_basic)
{
    char dst[64];
    ASSERT_OK(odfs_iso_name_to_display("FILE.TXT;1", 10, dst, sizeof(dst), 0));
    ASSERT_STR_EQ(dst, "FILE.TXT");
}

TEST(iso_name_lowercase)
{
    char dst[64];
    ASSERT_OK(odfs_iso_name_to_display("README.TXT;1", 12, dst, sizeof(dst), 1));
    ASSERT_STR_EQ(dst, "readme.txt");
}

TEST(iso_name_dir_trailing_dot)
{
    char dst[64];
    /* directory names have trailing dot, no version */
    ASSERT_OK(odfs_iso_name_to_display("SUBDIR.", 7, dst, sizeof(dst), 1));
    ASSERT_STR_EQ(dst, "subdir");
}

TEST(sanitize_name_controls)
{
    char name[] = "foo\x01/bar:baz";
    odfs_sanitize_name(name, sizeof(name) - 1, '_');
    ASSERT_STR_EQ(name, "foo__bar_baz");
}

TEST_MAIN()
