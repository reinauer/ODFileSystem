/*
 * freestanding.c - minimal libc for freestanding handlers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The OS4 newlib static libc.a contains only stubs that call through
 * the INewlib interface, which is set up by the C runtime startup the
 * handler cannot use (see start.c). Provide the handful of string
 * functions the handler and core library reference.
 * AROS also uses these routines so its handler needs no stdc.library
 * initialization or process startup.
 *
 * Compiled with -fno-builtin -fno-tree-loop-distribute-patterns so the
 * compiler cannot turn these loops back into calls to themselves.
 */

#include <stddef.h>
#include <string.h>
#include "amiga_target_compat.h"
#include <proto/exec.h>

/* Word accesses through this type may alias any object. */
typedef unsigned long __attribute__((__may_alias__)) word_t;

#define WSIZE sizeof(word_t)
#define WMASK (WSIZE - 1)
#define ONES  (~(word_t)0 / 0xff)
#define HIGHS (ONES << 7)
#define HAS_ZERO(w) (((w) - ONES) & ~(w) & HIGHS)

/* Below this an exec call costs more than its tuned copy saves. */
#define COPYMEM_MIN 64

/* Word copies need both pointers at the same offset within a word. */
static int co_aligned(const void *a, const void *b)
{
    return (((size_t)a ^ (size_t)b) & WMASK) == 0;
}

static void copy_fwd(unsigned char *d, const unsigned char *s, size_t n)
{
    if (n >= WSIZE && co_aligned(d, s)) {
        while ((size_t)d & WMASK) {
            *d++ = *s++;
            n--;
        }
        while (n >= 4 * WSIZE) {
            ((word_t *)d)[0] = ((const word_t *)s)[0];
            ((word_t *)d)[1] = ((const word_t *)s)[1];
            ((word_t *)d)[2] = ((const word_t *)s)[2];
            ((word_t *)d)[3] = ((const word_t *)s)[3];
            d += 4 * WSIZE;
            s += 4 * WSIZE;
            n -= 4 * WSIZE;
        }
        while (n >= WSIZE) {
            *(word_t *)d = *(const word_t *)s;
            d += WSIZE;
            s += WSIZE;
            n -= WSIZE;
        }
    }
    while (n--)
        *d++ = *s++;
}

/* d and s point one past the end of their regions. */
static void copy_bwd(unsigned char *d, const unsigned char *s, size_t n)
{
    if (n >= WSIZE && co_aligned(d, s)) {
        while ((size_t)d & WMASK) {
            *--d = *--s;
            n--;
        }
        while (n >= WSIZE) {
            d -= WSIZE;
            s -= WSIZE;
            n -= WSIZE;
            *(word_t *)d = *(const word_t *)s;
        }
    }
    while (n--)
        *--d = *--s;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    /* struct assignment may copy an object onto itself */
    if (dst == src)
        return dst;
    if (n >= COPYMEM_MIN)
        CopyMem((APTR)src, dst, n);
    else
        copy_fwd(dst, src, n);
    return dst;
}

/* GCC may emit memmove even when the source never calls it: it turns
 * overlapping copy loops into memmove calls (gcc 16 does so in
 * mount_volume). Like memcpy, memset and memcmp, a freestanding
 * environment has to provide it. */
void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;

    if (d == s)
        return dst;
    /* CopyMem does not support overlap, so only hand it disjoint
     * regions; an overlap with d below s still copies forward. */
    if ((size_t)d - (size_t)s >= n) {
        if ((size_t)s - (size_t)d >= n)
            return memcpy(dst, src, n);
        copy_fwd(d, s, n);
    } else {
        copy_bwd(d + n, s + n, n);
    }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;
    unsigned char b = (unsigned char)c;

    if (n >= 2 * WSIZE) {
        word_t w = ONES * b;

        while ((size_t)d & WMASK) {
            *d++ = b;
            n--;
        }
        while (n >= 4 * WSIZE) {
            ((word_t *)d)[0] = w;
            ((word_t *)d)[1] = w;
            ((word_t *)d)[2] = w;
            ((word_t *)d)[3] = w;
            d += 4 * WSIZE;
            n -= 4 * WSIZE;
        }
        while (n >= WSIZE) {
            *(word_t *)d = w;
            d += WSIZE;
            n -= WSIZE;
        }
    }
    while (n--)
        *d++ = b;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *pa = a;
    const unsigned char *pb = b;

    /* skip equal words; the byte loop then finds the first difference */
    if (n >= WSIZE && co_aligned(pa, pb)) {
        while ((size_t)pa & WMASK) {
            if (*pa != *pb)
                return *pa - *pb;
            pa++;
            pb++;
            n--;
        }
        while (n >= WSIZE && *(const word_t *)pa == *(const word_t *)pb) {
            pa += WSIZE;
            pb += WSIZE;
            n -= WSIZE;
        }
    }
    while (n--) {
        if (*pa != *pb)
            return *pa - *pb;
        pa++;
        pb++;
    }
    return 0;
}

size_t strlen(const char *s)
{
    const char *p = s;
    const word_t *w;

    while ((size_t)p & WMASK) {
        if (*p == '\0')
            return (size_t)(p - s);
        p++;
    }
    /* an aligned word never straddles a page, so reading past the
     * terminator within it is safe */
    for (w = (const word_t *)p; !HAS_ZERO(*w); w++)
        ;
    for (p = (const char *)w; *p; p++)
        ;
    return (size_t)(p - s);
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

char *strchr(const char *s, int c)
{
    char ch = (char)c;

    for (;; s++) {
        if (*s == ch)
            return (char *)s;
        if (*s == '\0')
            return NULL;
    }
}

char *strncpy(char *dst, const char *src, size_t n)
{
    char *d = dst;

    while (n && *src) {
        *d++ = *src++;
        n--;
    }
    memset(d, 0, n);
    return dst;
}
