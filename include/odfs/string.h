/*
 * string.h - small string helpers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef ODFS_STRING_H
#define ODFS_STRING_H

static inline unsigned char odfs_ascii_tolower(unsigned char ch)
{
    if (ch >= 'A' && ch <= 'Z')
        return (unsigned char)(ch + ('a' - 'A'));
    return ch;
}

static inline int odfs_strcasecmp(const char *a, const char *b)
{
    unsigned char ca;
    unsigned char cb;

    for (;;) {
        ca = odfs_ascii_tolower((unsigned char)*a++);
        cb = odfs_ascii_tolower((unsigned char)*b++);
        if (ca != cb || ca == '\0')
            return (int)ca - (int)cb;
    }
}

#endif /* ODFS_STRING_H */
