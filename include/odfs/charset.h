/*
 * odfs/charset.h — character conversion
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef ODFS_CHARSET_H
#define ODFS_CHARSET_H

#include "odfs/config.h"
#include "odfs/error.h"
#include <stdint.h>
#include <stddef.h>

/*
 * What a character that cannot be converted is replaced with. Not '?':
 * that is an AmigaDOS pattern character, and List ALL on AROS never
 * comes back from a directory that has one in its name.
 */
#define ODFS_CHAR_REPLACEMENT '_'

/*
 * Convert UCS-2 / UTF-16 big-endian to UTF-8.
 *   src      — UCS-2BE / UTF-16BE input buffer
 *   src_len  — input length in bytes (must be even)
 *   dst      — UTF-8 output buffer
 *   dst_size — size of dst buffer
 *   out_len  — receives number of bytes written (excluding NUL)
 *
 * Output is always NUL-terminated if dst_size > 0.
 * Unpaired surrogates and unmappable characters are replaced with '_'.
 */
odfs_err_t odfs_ucs2be_to_utf8(const uint8_t *src, size_t src_len,
                                  char *dst, size_t dst_size,
                                  size_t *out_len);

/*
 * Convert Mac Roman to UTF-8.
 *   src      - Mac Roman input buffer
 *   src_len  - input length in bytes
 *   dst      - UTF-8 output buffer
 *   dst_size - size of dst buffer
 *   out_len  - receives number of bytes written (excluding NUL)
 *
 * Output is always NUL-terminated if dst_size > 0.
 * Control characters are replaced with '_'.
 */
odfs_err_t odfs_mac_roman_to_utf8(const uint8_t *src, size_t src_len,
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

#if ODFS_FEATURE_CHARSET_BUILTIN
/*
 * Convert a UTF-8 string to ISO 8859-1 (Latin-1) in place and return
 * its new length; the result is never longer than the input.
 *
 * A character Latin-1 does not have becomes '_'. A byte that is not
 * part of a well-formed UTF-8 sequence is kept as it is, so a string
 * that already is in an 8-bit character set, such as a Rock Ridge name
 * written by an Amiga mastering tool, passes through unchanged.
 *
 * With ODFS_FEATURE_CHARSET_COMPOSE, a combining accent merges with
 * the letter before it where Latin-1 has the accented letter: "u"
 * followed by U+0308 becomes u-diaeresis.
 */
size_t odfs_utf8_to_latin1(char *s);
#endif

/*
 * The character set a mount gives names in. The backends decode every
 * name to UTF-8; a mount that asks for another set has node names,
 * symbolic link targets and the volume name converted to it.
 */
#define ODFS_NAMES_UTF8   0u
#define ODFS_NAMES_LATIN1 1u  /* ISO 8859-1, see odfs_utf8_to_latin1() */

/* convert a UTF-8 name in place to character set charset */
static inline void odfs_name_to_charset(char *name, unsigned int charset)
{
#if ODFS_FEATURE_CHARSET_BUILTIN
    if (charset == ODFS_NAMES_LATIN1)
        odfs_utf8_to_latin1(name);
#else
    (void)name;
    (void)charset;
#endif
}

#if !defined(AMIGA)
/*
 * Host-only fallback: replace non-printable / non-Amiga-safe
 * characters in a UTF-8 string with a replacement char.
 */
void odfs_sanitize_name(char *name, size_t len, char replacement);
#endif

/*
 * Convert a POSIX-style symlink target to AmigaDOS path syntax.
 * "." components vanish, each surviving leading ".." becomes a "/"
 * (AmigaDOS parent reference), and an absolute path is rooted with ":".
 * Interior ".." components are collapsed textually first ("a/../b" →
 * "b"), so parent references only remain where AmigaDOS can express
 * them. Output is NUL-terminated; an empty result means "current
 * directory" (e.g. for a "." target).
 */
odfs_err_t odfs_posix_to_amiga_path(const char *posix,
                                    char *out, size_t out_size);

#endif /* ODFS_CHARSET_H */
