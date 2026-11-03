/*
 * string.h - small string helpers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef ODFS_STRING_H
#define ODFS_STRING_H

#include <stddef.h>

static inline unsigned char odfs_ascii_tolower(unsigned char ch)
{
    if (ch >= 'A' && ch <= 'Z')
        return (unsigned char)(ch + ('a' - 'A'));
    return ch;
}

/*
 * Hash of the name, equal for names odfs_strcasecmp() calls equal.
 * Setting bit 5 folds 'A'-'Z' onto 'a'-'z' in one instruction; it
 * also merges a few other byte pairs, which only costs the rare extra
 * compare after a hash match. Rotate and xor rather than a
 * multiplicative hash: GCC turns any multiply, even one written as
 * shifts, into a libgcc call per character on the 68000. The final
 * shifts mix the leading characters into the low bits that pick a
 * hash bucket.
 */
static inline unsigned int odfs_strcasehash(const char *s)
{
    unsigned int hash = 0;
    unsigned int c;

    while ((c = (unsigned char)*s++) != 0) {
        hash = (hash << 5) | (hash >> 27);
        hash ^= c | 0x20u;
    }
    hash ^= hash >> 16;
    hash ^= hash >> 8;
    return hash;
}

/* odfs_strcasehash() of the len bytes at s, which need no terminator */
static inline unsigned int odfs_strcasehash_len(const char *s, size_t len)
{
    unsigned int hash = 0;

    while (len--) {
        hash = (hash << 5) | (hash >> 27);
        hash ^= (unsigned char)*s++ | 0x20u;
    }
    hash ^= hash >> 16;
    hash ^= hash >> 8;
    return hash;
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
