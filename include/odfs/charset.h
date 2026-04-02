/*
 * odfs/charset.h — character conversion
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef ODFS_CHARSET_H
#define ODFS_CHARSET_H

#include "odfs/error.h"
#include <stdint.h>
#include <stddef.h>

/*
 * Convert UCS-2 big-endian to UTF-8.
 *   src      — UCS-2BE input buffer
 *   src_len  — input length in bytes (must be even)
 *   dst      — UTF-8 output buffer
 *   dst_size — size of dst buffer
 *   out_len  — receives number of bytes written (excluding NUL)
 *
 * Output is always NUL-terminated if dst_size > 0.
 * Unmappable characters are replaced with '?'.
 */
odfs_err_t odfs_ucs2be_to_utf8(const uint8_t *src, size_t src_len,
                                  char *dst, size_t dst_size,
                                  size_t *out_len);

/*
 * Convert a raw ISO 9660 d-character / a-character name to a
 * display-friendly form (strip version number, lowercase optionally).
 *   src      — raw ISO name
 *   src_len  — length of src
 *   dst      — output buffer
 *   dst_size — size of dst buffer
 *   lowercase — if nonzero, lowercase ASCII letters
 */
odfs_err_t odfs_iso_name_to_display(const char *src, size_t src_len,
                                       char *dst, size_t dst_size,
                                       int lowercase);

/*
 * Fallback substitution: replace non-printable / non-Amiga-safe
 * characters in a UTF-8 string with a replacement char.
 */
void odfs_sanitize_name(char *name, size_t len, char replacement);

#endif /* ODFS_CHARSET_H */
