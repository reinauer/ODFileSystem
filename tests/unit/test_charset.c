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
    size_t len = 99;

    /* callers use the length and name even when conversion fails */
    memset(dst, 'X', sizeof(dst));
    ASSERT_ERR(odfs_ucs2be_to_utf8(src, 3, dst, sizeof(dst), &len),
               ODFS_ERR_INVAL);
    ASSERT_EQ(dst[0], '\0');
    ASSERT_EQ(len, 0);
}

TEST(ucs2be_surrogate_pairs)
{
    /* U+1F600 (D83D DE00) -> F0 9F 98 80, followed by 'A' */
    const uint8_t pair[] = { 0xD8, 0x3D, 0xDE, 0x00, 0x00, 'A' };
    /* lone high surrogate, 'B', lone low surrogate, high at the end */
    const uint8_t lone[] = { 0xD8, 0x00, 0x00, 'B', 0xDC, 0x01, 0xDB, 0xFF };
    char dst[64];
    size_t len = 0;

    ASSERT_OK(odfs_ucs2be_to_utf8(pair, sizeof(pair), dst, sizeof(dst), &len));
    ASSERT_EQ(len, 5);
    ASSERT_EQ((unsigned char)dst[0], 0xF0);
    ASSERT_EQ((unsigned char)dst[1], 0x9F);
    ASSERT_EQ((unsigned char)dst[2], 0x98);
    ASSERT_EQ((unsigned char)dst[3], 0x80);
    ASSERT_EQ(dst[4], 'A');
    ASSERT_EQ(dst[5], '\0');

    ASSERT_OK(odfs_ucs2be_to_utf8(lone, sizeof(lone), dst, sizeof(dst), &len));
    ASSERT_STR_EQ(dst, "_B__");
    ASSERT_EQ(len, 4);

    /* a pair that does not fit is left out whole */
    ASSERT_OK(odfs_ucs2be_to_utf8(pair, sizeof(pair), dst, 4, &len));
    ASSERT_EQ(len, 0);
    ASSERT_EQ(dst[0], '\0');
}

TEST(mac_roman_ascii)
{
    const uint8_t src[] = { 'H', 'e', 'l', 'l', 'o' };
    char dst[64];
    size_t len;

    ASSERT_OK(odfs_mac_roman_to_utf8(src, sizeof(src), dst, sizeof(dst), &len));
    ASSERT_STR_EQ(dst, "Hello");
    ASSERT_EQ(len, 5);
}

TEST(mac_roman_high_chars)
{
    const uint8_t src[] = { 0x80, 0x8E, 0xDB, 0xD0 };
    char dst[64];
    size_t len;

    ASSERT_OK(odfs_mac_roman_to_utf8(src, sizeof(src), dst, sizeof(dst), &len));
    ASSERT_EQ((unsigned char)dst[0], 0xC3);
    ASSERT_EQ((unsigned char)dst[1], 0x84);
    ASSERT_EQ((unsigned char)dst[2], 0xC3);
    ASSERT_EQ((unsigned char)dst[3], 0xA9);
    ASSERT_EQ((unsigned char)dst[4], 0xE2);
    ASSERT_EQ((unsigned char)dst[5], 0x82);
    ASSERT_EQ((unsigned char)dst[6], 0xAC);
    ASSERT_EQ((unsigned char)dst[7], 0xE2);
    ASSERT_EQ((unsigned char)dst[8], 0x80);
    ASSERT_EQ((unsigned char)dst[9], 0x93);
    ASSERT_EQ(dst[10], '\0');
    ASSERT_EQ(len, 10);
}

TEST(mac_roman_controls)
{
    const uint8_t src[] = { 'A', 0x01, 'B' };
    char dst[64];
    size_t len;

    ASSERT_OK(odfs_mac_roman_to_utf8(src, sizeof(src), dst, sizeof(dst), &len));
    ASSERT_STR_EQ(dst, "A_B");
    ASSERT_EQ(len, 3);
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

TEST(utf8_to_latin1_ascii)
{
    char s[] = "README.TXT";

    ASSERT_EQ(odfs_utf8_to_8bit(s, NULL), 10);
    ASSERT_STR_EQ(s, "README.TXT");
}

TEST(utf8_to_latin1_converts_latin1_range)
{
    /* "Gr" U+00FC U+00DF "e" U+00A0 U+00FF */
    char s[] = "Gr\xC3\xBC\xC3\x9F" "e\xC2\xA0\xC3\xBF";

    ASSERT_EQ(odfs_utf8_to_8bit(s, NULL), 7);
    ASSERT_STR_EQ(s, "Gr\xFC\xDF" "e\xA0\xFF");
}

TEST(utf8_to_latin1_replaces_other_characters)
{
    /* U+0100, U+20AC, U+65E5 U+672C, U+1F600: one '_' each */
    char s[] = "a\xC4\x80" "b\xE2\x82\xAC" "c\xE6\x97\xA5\xE6\x9C\xAC"
               "d\xF0\x9F\x98\x80" "e";

    ASSERT_EQ(odfs_utf8_to_8bit(s, NULL), 10);
    ASSERT_STR_EQ(s, "a_b_c__d_e");
}

TEST(utf8_to_latin1_replaces_c1_controls)
{
    /* U+0080, U+009B (CSI), U+009F */
    char s[] = "\xC2\x80\xC2\x9B\xC2\x9F";

    ASSERT_EQ(odfs_utf8_to_8bit(s, NULL), 3);
    ASSERT_STR_EQ(s, "___");
}

/* a name that is Latin-1 already must come out as it went in */
TEST(utf8_to_latin1_keeps_raw_8bit_names)
{
    /* "Grüße", "Ärger.txt", "Señor Ñ", "café" and "ü" at the very end */
    char a[] = "Gr\xFC\xDF" "e";
    char b[] = "\xC4rger.txt";
    char c[] = "Se\xF1or \xD1";
    char d[] = "caf\xE9";
    char e[] = "Men\xFC";

    ASSERT_EQ(odfs_utf8_to_8bit(a, NULL), 5);
    ASSERT_STR_EQ(a, "Gr\xFC\xDF" "e");
    odfs_utf8_to_8bit(b, NULL);
    ASSERT_STR_EQ(b, "\xC4rger.txt");
    odfs_utf8_to_8bit(c, NULL);
    ASSERT_STR_EQ(c, "Se\xF1or \xD1");
    odfs_utf8_to_8bit(d, NULL);
    ASSERT_STR_EQ(d, "caf\xE9");
    odfs_utf8_to_8bit(e, NULL);
    ASSERT_STR_EQ(e, "Men\xFC");
}

TEST(utf8_to_latin1_keeps_malformed_sequences)
{
    /* lone continuation, lead cut short by the end, lead cut short by
     * ASCII, and the bytes UTF-8 never uses */
    char a[] = "a\x80" "b\xBF";
    char b[] = "x\xE2\x82";
    char c[] = "x\xF0\x9F\x98" "y";
    char d[] = "\xC0\x80\xC1\xBF\xF5\x80\xFF";

    odfs_utf8_to_8bit(a, NULL);
    ASSERT_STR_EQ(a, "a\x80" "b\xBF");
    odfs_utf8_to_8bit(b, NULL);
    ASSERT_STR_EQ(b, "x\xE2\x82");
    odfs_utf8_to_8bit(c, NULL);
    ASSERT_STR_EQ(c, "x\xF0\x9F\x98" "y");
    odfs_utf8_to_8bit(d, NULL);
    ASSERT_STR_EQ(d, "\xC0\x80\xC1\xBF\xF5\x80\xFF");
}

/* an overlong form must not turn into the character it spells, least
 * of all into a NUL or a '/' */
TEST(utf8_to_latin1_keeps_overlong_forms)
{
    char a[] = "a\xE0\x80\x80" "b";      /* U+0000 in three bytes */
    char b[] = "a\xE0\x80\xAF" "b";      /* '/' in three bytes */
    char c[] = "a\xF0\x80\x83\xA9" "b";  /* U+00E9 in four bytes */

    ASSERT_EQ(odfs_utf8_to_8bit(a, NULL), 5);
    ASSERT_STR_EQ(a, "a\xE0\x80\x80" "b");
    ASSERT_EQ(odfs_utf8_to_8bit(b, NULL), 5);
    ASSERT_STR_EQ(b, "a\xE0\x80\xAF" "b");
    ASSERT_EQ(odfs_utf8_to_8bit(c, NULL), 6);
    ASSERT_STR_EQ(c, "a\xF0\x80\x83\xA9" "b");
}

/* HFS+ stores "u-diaeresis" as "u" followed by U+0308 */
TEST(utf8_to_latin1_composes_accents)
{
#if ODFS_FEATURE_CHARSET_COMPOSE
    /* A-diaeresis "rger", "Gru" + diaeresis + sharp s + "e",
     * "cafe" + acute, "gar" c-cedilla "on", "A" + ring, "n" + tilde,
     * "y" + diaeresis, "Y" + acute */
    char a[] = "A\xCC\x88rger";
    char b[] = "Gru\xCC\x88\xC3\x9F" "e";
    char c[] = "cafe\xCC\x81";
    char d[] = "garc\xCC\xA7on";
    char e[] = "A\xCC\x8A n\xCC\x83 y\xCC\x88 Y\xCC\x81";
    /* every capital with every mark it takes */
    char f[] = "A\xCC\x80" "A\xCC\x81" "A\xCC\x82" "A\xCC\x83"
               "A\xCC\x88" "A\xCC\x8A" "C\xCC\xA7"
               "E\xCC\x80" "E\xCC\x81" "E\xCC\x82" "E\xCC\x88"
               "I\xCC\x80" "I\xCC\x81" "I\xCC\x82" "I\xCC\x88"
               "N\xCC\x83"
               "O\xCC\x80" "O\xCC\x81" "O\xCC\x82" "O\xCC\x83"
               "O\xCC\x88"
               "U\xCC\x80" "U\xCC\x81" "U\xCC\x82" "U\xCC\x88"
               "Y\xCC\x81";
    /* no such letter in Latin-1: s + caron, Y + diaeresis, q + acute;
     * a mark with nothing before it, and one after a non-letter */
    char g[] = "s\xCC\x8C Y\xCC\x88 q\xCC\x81";
    char h[] = "\xCC\x88" "a";
    char k[] = "1\xCC\x81 @\xCC\x80 `\xCC\x80";
    /* the mark only joins the letter right before it */
    char l[] = "a\xCC\x88\xCC\x81";

    ASSERT_EQ(odfs_utf8_to_8bit(a, NULL), 5);
    ASSERT_STR_EQ(a, "\xC4rger");
    odfs_utf8_to_8bit(b, NULL);
    ASSERT_STR_EQ(b, "Gr\xFC\xDF" "e");
    odfs_utf8_to_8bit(c, NULL);
    ASSERT_STR_EQ(c, "caf\xE9");
    odfs_utf8_to_8bit(d, NULL);
    ASSERT_STR_EQ(d, "gar\xE7on");
    odfs_utf8_to_8bit(e, NULL);
    ASSERT_STR_EQ(e, "\xC5 \xF1 \xFF \xDD");
    odfs_utf8_to_8bit(f, NULL);
    ASSERT_STR_EQ(f, "\xC0\xC1\xC2\xC3\xC4\xC5\xC7\xC8\xC9\xCA\xCB"
                     "\xCC\xCD\xCE\xCF\xD1\xD2\xD3\xD4\xD5\xD6"
                     "\xD9\xDA\xDB\xDC\xDD");
    odfs_utf8_to_8bit(g, NULL);
    ASSERT_STR_EQ(g, "s_ Y_ q_");
    odfs_utf8_to_8bit(h, NULL);
    ASSERT_STR_EQ(h, "_a");
    odfs_utf8_to_8bit(k, NULL);
    ASSERT_STR_EQ(k, "1_ @_ `_");
    odfs_utf8_to_8bit(l, NULL);
    ASSERT_STR_EQ(l, "\xE4_");
#endif
}

#if ODFS_FEATURE_CHARSET_EXTERNAL
/* ISO 8859-2, 0xA0 to 0xFF */
static const uint16_t iso8859_2[ODFS_CHARSET_MAP_SIZE] = {
    0x00A0, 0x0104, 0x02D8, 0x0141, 0x00A4, 0x013D, 0x015A, 0x00A7,
    0x00A8, 0x0160, 0x015E, 0x0164, 0x0179, 0x00AD, 0x017D, 0x017B,
    0x00B0, 0x0105, 0x02DB, 0x0142, 0x00B4, 0x013E, 0x015B, 0x02C7,
    0x00B8, 0x0161, 0x015F, 0x0165, 0x017A, 0x02DD, 0x017E, 0x017C,
    0x0154, 0x00C1, 0x00C2, 0x0102, 0x00C4, 0x0139, 0x0106, 0x00C7,
    0x010C, 0x00C9, 0x0118, 0x00CB, 0x011A, 0x00CD, 0x00CE, 0x010E,
    0x0110, 0x0143, 0x0147, 0x00D3, 0x00D4, 0x0150, 0x00D6, 0x00D7,
    0x0158, 0x016E, 0x00DA, 0x0170, 0x00DC, 0x00DD, 0x0162, 0x00DF,
    0x0155, 0x00E1, 0x00E2, 0x0103, 0x00E4, 0x013A, 0x0107, 0x00E7,
    0x010D, 0x00E9, 0x0119, 0x00EB, 0x011B, 0x00ED, 0x00EE, 0x010F,
    0x0111, 0x0144, 0x0148, 0x00F3, 0x00F4, 0x0151, 0x00F6, 0x00F7,
    0x0159, 0x016F, 0x00FA, 0x0171, 0x00FC, 0x00FD, 0x0163, 0x02D9
};
#endif

TEST(utf8_to_8bit_follows_a_map)
{
#if ODFS_FEATURE_CHARSET_EXTERNAL
    /* U+017B U+00F3 U+0142 U+0107 ".txt", and U+010C "e" U+0161
     * "tina" */
    char a[] = "\xC5\xBB\xC3\xB3\xC5\x82\xC4\x87.txt";
    char b[] = "\xC4\x8C" "e\xC5\xA1tina";
    /* in Latin-1 but not in this set: n-tilde, a-ring, yen; and
     * outside both: U+20AC, U+65E5 */
    char c[] = "Se\xC3\xB1or \xC3\xA5 \xC2\xA5 \xE2\x82\xAC \xE6\x97\xA5";
    /* where the set and Latin-1 agree */
    char d[] = "Gr\xC3\xBC\xC3\x9F" "e";
    /* bytes that are not UTF-8 stay, as they do for Latin-1 */
    char e[] = "\xAF\xF3\xB3\xE6.txt";

    ASSERT_EQ(odfs_utf8_to_8bit(a, iso8859_2), 8);
    ASSERT_STR_EQ(a, "\xAF\xF3\xB3\xE6.txt");
    odfs_utf8_to_8bit(b, iso8859_2);
    ASSERT_STR_EQ(b, "\xC8" "e\xB9tina");
    odfs_utf8_to_8bit(c, iso8859_2);
    ASSERT_STR_EQ(c, "Se_or _ _ _ _");
    odfs_utf8_to_8bit(d, iso8859_2);
    ASSERT_STR_EQ(d, "Gr\xFC\xDF" "e");
    odfs_utf8_to_8bit(e, iso8859_2);
    ASSERT_STR_EQ(e, "\xAF\xF3\xB3\xE6.txt");
#endif
}

/* a 0 in the map is a byte without a character, never a match */
TEST(utf8_to_8bit_skips_unused_map_entries)
{
#if ODFS_FEATURE_CHARSET_EXTERNAL
    static uint16_t map[ODFS_CHARSET_MAP_SIZE];
    /* U+20AC, U+00A4, U+00E9 */
    char s[] = "5\xE2\x82\xAC \xC2\xA4 \xC3\xA9";

    /* Latin-1 with the Euro sign in place of the currency sign */
    map[0xA4 - ODFS_CHARSET_MAP_FIRST] = 0x20AC;
    ASSERT_EQ(odfs_utf8_to_8bit(s, map), 6);
    ASSERT_STR_EQ(s, "5\xA4 _ _");
#endif
}

/* an accent merges only into a letter the set has */
TEST(utf8_to_8bit_composes_accents_through_a_map)
{
#if ODFS_FEATURE_CHARSET_EXTERNAL && ODFS_FEATURE_CHARSET_COMPOSE
    /* A + diaeresis and e + acute are in ISO 8859-2; a + grave and
     * n + tilde are not */
    char s[] = "A\xCC\x88 e\xCC\x81 a\xCC\x80 n\xCC\x83";

    odfs_utf8_to_8bit(s, iso8859_2);
    ASSERT_STR_EQ(s, "\xC4 \xE9 a_ n_");
#endif
}

TEST(utf8_to_latin1_empty)
{
    char s[] = "";

    ASSERT_EQ(odfs_utf8_to_8bit(s, NULL), 0);
    ASSERT_STR_EQ(s, "");
}

TEST_MAIN()
