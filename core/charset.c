/*
 * charset.c — built-in character conversion
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "odfs/charset.h"
#include <string.h>

static int odfs_utf8_append(uint32_t cp, char *dst, size_t dst_size, size_t *di)
{
    if (cp < 0x80) {
        if (*di + 1 >= dst_size)
            return 0;
        dst[(*di)++] = (char)cp;
    } else if (cp < 0x800) {
        if (*di + 2 >= dst_size)
            return 0;
        dst[(*di)++] = (char)(0xC0 | (cp >> 6));
        dst[(*di)++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        if (*di + 3 >= dst_size)
            return 0;
        dst[(*di)++] = (char)(0xE0 | (cp >> 12));
        dst[(*di)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        dst[(*di)++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp <= 0x10FFFF) {
        if (*di + 4 >= dst_size)
            return 0;
        dst[(*di)++] = (char)(0xF0 | (cp >> 18));
        dst[(*di)++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        dst[(*di)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        dst[(*di)++] = (char)(0x80 | (cp & 0x3F));
    } else {
        if (*di + 1 >= dst_size)
            return 0;
        dst[(*di)++] = '?';
    }

    return 1;
}

odfs_err_t odfs_ucs2be_to_utf8(const uint8_t *src, size_t src_len,
                                  char *dst, size_t dst_size,
                                  size_t *out_len)
{
    size_t di = 0;
    size_t si;

    if (!src || !dst || dst_size == 0)
        return ODFS_ERR_INVAL;

    /* UCS-2 is 2 bytes per code unit */
    if (src_len & 1)
        return ODFS_ERR_INVAL;

    for (si = 0; si + 1 < src_len; si += 2) {
        uint16_t cp = ((uint16_t)src[si] << 8) | src[si + 1];

        if (cp == 0)
            break; /* NUL terminator */

        if (!odfs_utf8_append(cp, dst, dst_size, &di))
            break;
    }

    dst[di] = '\0';
    if (out_len)
        *out_len = di;

    return ODFS_OK;
}

odfs_err_t odfs_mac_roman_to_utf8(const uint8_t *src, size_t src_len,
                                  char *dst, size_t dst_size,
                                  size_t *out_len)
{
    static const uint16_t mac_roman_high[128] = {
        0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1,
        0x00E0, 0x00E2, 0x00E4, 0x00E3, 0x00E5, 0x00E7, 0x00E9, 0x00E8,
        0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3,
        0x00F2, 0x00F4, 0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC,
        0x2020, 0x00B0, 0x00A2, 0x00A3, 0x00A7, 0x2022, 0x00B6, 0x00DF,
        0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8,
        0x221E, 0x00B1, 0x2264, 0x2265, 0x00A5, 0x00B5, 0x2202, 0x2211,
        0x220F, 0x03C0, 0x222B, 0x00AA, 0x00BA, 0x03A9, 0x00E6, 0x00F8,
        0x00BF, 0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB,
        0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5, 0x0152, 0x0153,
        0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0x00F7, 0x25CA,
        0x00FF, 0x0178, 0x2044, 0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02,
        0x2021, 0x00B7, 0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1,
        0x00CB, 0x00C8, 0x00CD, 0x00CE, 0x00CF, 0x00CC, 0x00D3, 0x00D4,
        0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9, 0x0131, 0x02C6, 0x02DC,
        0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7
    };
    size_t di = 0;
    size_t si;

    if (!src || !dst || dst_size == 0)
        return ODFS_ERR_INVAL;

    for (si = 0; si < src_len; si++) {
        uint8_t c = src[si];
        uint32_t cp;

        if (c < 0x20)
            cp = '?';
        else if (c < 0x80)
            cp = c;
        else
            cp = mac_roman_high[c - 0x80];

        if (!odfs_utf8_append(cp, dst, dst_size, &di))
            break;
    }

    dst[di] = '\0';
    if (out_len)
        *out_len = di;

    return ODFS_OK;
}

odfs_err_t odfs_iso_name_to_display(const char *src, size_t src_len,
                                       char *dst, size_t dst_size,
                                       int lowercase)
{
    size_t i, di = 0;

    if (!src || !dst || dst_size == 0)
        return ODFS_ERR_INVAL;

    for (i = 0; i < src_len && di + 1 < dst_size; i++) {
        char c = src[i];

        /* stop at version separator */
        if (c == ';')
            break;

        /* strip trailing dot for directories (single dot at end) */
        if (c == '.' && i + 1 == src_len)
            break;

        if (lowercase && c >= 'A' && c <= 'Z')
            c = c - 'A' + 'a';

        dst[di++] = c;
    }

    dst[di] = '\0';
    return ODFS_OK;
}

void odfs_sanitize_name(char *name, size_t len, char replacement)
{
    for (size_t i = 0; i < len && name[i] != '\0'; i++) {
        unsigned char c = (unsigned char)name[i];
        /* replace control characters and Amiga-problematic chars */
        if (c < 0x20 || c == '/' || c == ':' || c == 0x7F)
            name[i] = replacement;
    }
}
