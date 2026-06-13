/*
 * freestanding.c - minimal libc for the AmigaOS 4 handler
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The OS4 newlib static libc.a contains only stubs that call through
 * the INewlib interface, which is set up by the C runtime startup the
 * handler cannot use (see start.c). Provide the handful of string
 * functions the handler and core library reference.
 *
 * Compiled with -fno-builtin so the compiler cannot turn these loops
 * back into calls to themselves.
 */

#include <stddef.h>
#include <string.h>

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;

    if (((size_t)d & 3u) == 0 && ((size_t)s & 3u) == 0) {
        while (n >= 4) {
            *(unsigned long *)d = *(const unsigned long *)s;
            d += 4;
            s += 4;
            n -= 4;
        }
    }
    while (n--)
        *d++ = *s++;
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;

    while (n--)
        *d++ = (unsigned char)c;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *pa = a;
    const unsigned char *pb = b;

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

    while (*p)
        p++;
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
    while (n--)
        *d++ = '\0';
    return dst;
}
