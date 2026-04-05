/*
 * printf_local.c - small local snprintf/vsnprintf for handler builds
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Keep a compact bounded formatter here so the handler does not pull in the
 * full libc stdio formatting stack.
 */

#include "odfs/printf.h"

#include <stdint.h>

enum {
    FMT_ALT       = 1u << 0,
    FMT_ZERO_PAD  = 1u << 1,
    FMT_LEFT      = 1u << 2,
    FMT_PLUS      = 1u << 3,
    FMT_SPACE     = 1u << 4,
    FMT_UPPER     = 1u << 5,
    FMT_NEGATIVE  = 1u << 6,
};

enum fmt_length {
    FMT_LEN_DEFAULT = 0,
    FMT_LEN_SHORT,
    FMT_LEN_LONG,
    FMT_LEN_LLONG,
    FMT_LEN_SIZE,
    FMT_LEN_PTRDIFF,
};

typedef struct fmt_buf {
    char *cur;
    char *end;
} fmt_buf_t;

static uint64_t udiv64_32(uint64_t value, uint32_t base, uint32_t *rem_out)
{
    uint32_t high = (uint32_t)(value >> 32);
    uint32_t low  = (uint32_t)value;
    uint32_t q_high = 0, q_low = 0;
    uint32_t rem = 0;
    int i;

    for (i = 31; i >= 0; i--) {
        rem = (rem << 1) | ((high >> i) & 1u);
        if (rem >= base) {
            rem -= base;
            q_high |= (1u << i);
        }
    }

    for (i = 31; i >= 0; i--) {
        rem = (rem << 1) | ((low >> i) & 1u);
        if (rem >= base) {
            rem -= base;
            q_low |= (1u << i);
        }
    }

    if (rem_out)
        *rem_out = rem;

    return ((uint64_t)q_high << 32) | (uint64_t)q_low;
}

static void fmt_putc(fmt_buf_t *buf, char ch, int *count)
{
    if (buf->cur < buf->end)
        *buf->cur++ = ch;
    ++*count;
}

static void fmt_pad(fmt_buf_t *buf, char ch, int width, int *count)
{
    while (width-- > 0)
        fmt_putc(buf, ch, count);
}

static void fmt_string(fmt_buf_t *buf,
                       const char *s,
                       unsigned int flags,
                       int width,
                       int precision,
                       int *count)
{
    size_t len;
    int pad;
    size_t i;
    const char *p;

    if (s == NULL)
        s = "(null)";

    for (p = s; *p != '\0'; ++p)
        continue;
    len = (size_t)(p - s);
    if (precision >= 0 && (size_t)precision < len)
        len = (size_t)precision;

    pad = width - (int)len;
    if ((flags & FMT_LEFT) == 0)
        fmt_pad(buf, ' ', pad, count);

    for (i = 0; i < len; ++i)
        fmt_putc(buf, s[i], count);

    if (flags & FMT_LEFT)
        fmt_pad(buf, ' ', pad, count);
}

static void fmt_unsigned(fmt_buf_t *buf,
                         uint64_t value,
                         unsigned int base,
                         unsigned int flags,
                         int width,
                         int precision,
                         const char *prefix,
                         int *count)
{
    char digits[32];
    const char *alphabet = (flags & FMT_UPPER) ? "0123456789ABCDEF"
                                               : "0123456789abcdef";
    int ndigits = 0;
    int prefix_len = 0;
    int zeroes;
    int total;
    char pad;
    int i;

    if (prefix != NULL) {
        while (prefix[prefix_len] != '\0')
            ++prefix_len;
    }

    if (value == 0) {
        if (precision != 0)
            digits[ndigits++] = '0';
    } else {
        do {
            uint32_t rem;
            value = udiv64_32(value, base, &rem);
            digits[ndigits++] = alphabet[rem];
        } while (value != 0);
    }

    zeroes = precision - ndigits;
    if (zeroes < 0)
        zeroes = 0;

    total = prefix_len + zeroes + ndigits;
    pad = ((flags & (FMT_ZERO_PAD | FMT_LEFT)) == FMT_ZERO_PAD &&
           precision < 0) ? '0' : ' ';

    if ((flags & FMT_LEFT) == 0 && pad == ' ')
        fmt_pad(buf, ' ', width - total, count);

    if (prefix != NULL) {
        for (i = 0; i < prefix_len; ++i)
            fmt_putc(buf, prefix[i], count);
    }

    if ((flags & FMT_LEFT) == 0 && pad == '0')
        fmt_pad(buf, '0', width - total, count);

    fmt_pad(buf, '0', zeroes, count);

    for (i = ndigits - 1; i >= 0; --i)
        fmt_putc(buf, digits[i], count);

    if (flags & FMT_LEFT)
        fmt_pad(buf, ' ', width - total, count);
}

static void fmt_signed(fmt_buf_t *buf,
                       int64_t value,
                       unsigned int flags,
                       int width,
                       int precision,
                       int *count)
{
    uint64_t uvalue;
    const char *prefix = NULL;
    char sign[2];

    if (value < 0) {
        flags |= FMT_NEGATIVE;
        uvalue = (uint64_t)(-(value + 1)) + 1;
        sign[0] = '-';
        sign[1] = '\0';
        prefix = sign;
    } else {
        uvalue = (uint64_t)value;
        if (flags & FMT_PLUS) {
            sign[0] = '+';
            sign[1] = '\0';
            prefix = sign;
        } else if (flags & FMT_SPACE) {
            sign[0] = ' ';
            sign[1] = '\0';
            prefix = sign;
        }
    }

    fmt_unsigned(buf, uvalue, 10, flags, width, precision, prefix, count);
}

static int kvsnprintf(fmt_buf_t *buf, const char *fmt, va_list ap)
{
    int count = 0;

    while (*fmt != '\0') {
        unsigned int flags = 0;
        enum fmt_length length = FMT_LEN_DEFAULT;
        int width = 0;
        int precision = -1;
        char conv;

        if (*fmt != '%') {
            fmt_putc(buf, *fmt++, &count);
            continue;
        }

        ++fmt;
        if (*fmt == '%') {
            fmt_putc(buf, *fmt++, &count);
            continue;
        }

        for (;;) {
            switch (*fmt) {
            case '#': flags |= FMT_ALT; ++fmt; continue;
            case '0': flags |= FMT_ZERO_PAD; ++fmt; continue;
            case '-': flags |= FMT_LEFT; ++fmt; continue;
            case '+': flags |= FMT_PLUS; ++fmt; continue;
            case ' ': flags |= FMT_SPACE; ++fmt; continue;
            default: break;
            }
            break;
        }

        if (*fmt == '*') {
            width = va_arg(ap, int);
            if (width < 0) {
                flags |= FMT_LEFT;
                width = -width;
            }
            ++fmt;
        } else {
            while (*fmt >= '0' && *fmt <= '9') {
                width = (width * 10) + (*fmt - '0');
                ++fmt;
            }
        }

        if (*fmt == '.') {
            precision = 0;
            ++fmt;
            if (*fmt == '*') {
                precision = va_arg(ap, int);
                ++fmt;
                if (precision < 0)
                    precision = -1;
            } else {
                while (*fmt >= '0' && *fmt <= '9') {
                    precision = (precision * 10) + (*fmt - '0');
                    ++fmt;
                }
            }
        }

        if (*fmt == 'h') {
            length = FMT_LEN_SHORT;
            ++fmt;
        } else if (*fmt == 'l') {
            ++fmt;
            if (*fmt == 'l') {
                length = FMT_LEN_LLONG;
                ++fmt;
            } else {
                length = FMT_LEN_LONG;
            }
        } else if (*fmt == 'z') {
            length = FMT_LEN_SIZE;
            ++fmt;
        } else if (*fmt == 't') {
            length = FMT_LEN_PTRDIFF;
            ++fmt;
        }

        conv = *fmt;
        if (conv == '\0')
            break;
        ++fmt;

        switch (conv) {
        case 'c': {
            int ch = va_arg(ap, int);
            if ((flags & FMT_LEFT) == 0)
                fmt_pad(buf, ' ', width - 1, &count);
            fmt_putc(buf, (char)ch, &count);
            if (flags & FMT_LEFT)
                fmt_pad(buf, ' ', width - 1, &count);
            break;
        }
        case 's':
            fmt_string(buf, va_arg(ap, const char *), flags, width, precision, &count);
            break;
        case 'd':
        case 'i':
            switch (length) {
            case FMT_LEN_LLONG:
                fmt_signed(buf, va_arg(ap, long long), flags, width, precision, &count);
                break;
            case FMT_LEN_LONG:
                fmt_signed(buf, va_arg(ap, long), flags, width, precision, &count);
                break;
            case FMT_LEN_SIZE:
                if (sizeof(size_t) == sizeof(unsigned long long))
                    fmt_signed(buf, (long long)va_arg(ap, size_t), flags, width, precision, &count);
                else
                    fmt_signed(buf, (long)va_arg(ap, size_t), flags, width, precision, &count);
                break;
            case FMT_LEN_PTRDIFF:
                if (sizeof(ptrdiff_t) == sizeof(long long))
                    fmt_signed(buf, (long long)va_arg(ap, ptrdiff_t), flags, width, precision, &count);
                else
                    fmt_signed(buf, (long)va_arg(ap, ptrdiff_t), flags, width, precision, &count);
                break;
            case FMT_LEN_SHORT:
                fmt_signed(buf, (short)va_arg(ap, int), flags, width, precision, &count);
                break;
            default:
                fmt_signed(buf, va_arg(ap, int), flags, width, precision, &count);
                break;
            }
            break;
        case 'u':
        case 'o':
        case 'x':
        case 'X': {
            uint64_t value;
            unsigned int base = (conv == 'o') ? 8u : (conv == 'u') ? 10u : 16u;
            const char *prefix = NULL;

            if (conv == 'X')
                flags |= FMT_UPPER;

            switch (length) {
            case FMT_LEN_LLONG:
                value = va_arg(ap, unsigned long long);
                break;
            case FMT_LEN_LONG:
                value = va_arg(ap, unsigned long);
                break;
            case FMT_LEN_SIZE:
                value = va_arg(ap, size_t);
                break;
            case FMT_LEN_PTRDIFF:
                value = (uint64_t)va_arg(ap, ptrdiff_t);
                break;
            case FMT_LEN_SHORT:
                value = (unsigned short)va_arg(ap, unsigned int);
                break;
            default:
                value = va_arg(ap, unsigned int);
                break;
            }

            if ((flags & FMT_ALT) && value != 0) {
                if (conv == 'o')
                    prefix = "0";
                else if (conv == 'x')
                    prefix = "0x";
                else if (conv == 'X')
                    prefix = "0X";
            }

            fmt_unsigned(buf, value, base, flags, width, precision, prefix, &count);
            break;
        }
        case 'p': {
            uintptr_t value = (uintptr_t)va_arg(ap, void *);
            fmt_unsigned(buf, value, 16, flags, width, precision, "0x", &count);
            break;
        }
        default:
            fmt_putc(buf, '%', &count);
            fmt_putc(buf, conv, &count);
            break;
        }
    }

    return count;
}

int odfs_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    fmt_buf_t out;
    int count;

    out.cur = buf;
    out.end = (buf != NULL && size != 0) ? (buf + size - 1) : buf;
    count = kvsnprintf(&out, fmt, ap);

    if (buf != NULL && size != 0)
        *out.cur = '\0';

    return count;
}

int odfs_snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    int count;

    va_start(ap, fmt);
    count = odfs_vsnprintf(buf, size, fmt, ap);
    va_end(ap);

    return count;
}
