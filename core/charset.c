/*
 * charset.c — built-in character conversion
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "odfs/charset.h"
#include <string.h>

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

        if (cp < 0x80) {
            if (di + 1 >= dst_size) break;
            dst[di++] = (char)cp;
        } else if (cp < 0x800) {
            if (di + 2 >= dst_size) break;
            dst[di++] = (char)(0xC0 | (cp >> 6));
            dst[di++] = (char)(0x80 | (cp & 0x3F));
        } else {
            if (di + 3 >= dst_size) break;
            dst[di++] = (char)(0xE0 | (cp >> 12));
            dst[di++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            dst[di++] = (char)(0x80 | (cp & 0x3F));
        }
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
