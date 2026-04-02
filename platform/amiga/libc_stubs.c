/*
 * libc_stubs.c — minimal C library stubs for handler builds
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The handler does not link against a full C runtime.
 * Provide the minimal symbols needed by core library code.
 * TODO: replace snprintf usage in log.c with RawDoFmt-based formatter.
 */

#include <exec/types.h>
#include <proto/exec.h>
#include <stdarg.h>

extern struct ExecBase *SysBase;

int __errno = 0;

void _exit(int status);
void _exit(int status)
{
    (void)status;
    /* handler should never call exit — hang rather than corrupt */
    for (;;)
        Wait(0);
}

void exit(int status);
void exit(int status)
{
    _exit(status);
}

/* minimal snprintf via RawDoFmt */
struct putch_data {
    char *buf;
    int   remaining;
};

static void putch_func(void)
{
    /* RawDoFmt callback: d0 = char, a3 = PutChData */
    register char ch __asm("d0");
    register struct putch_data *pd __asm("a3");

    if (pd->remaining > 1) {
        *pd->buf++ = ch;
        pd->remaining--;
    }
}

int vsnprintf(char *buf, unsigned long size, const char *fmt, va_list ap);
int vsnprintf(char *buf, unsigned long size, const char *fmt, va_list ap)
{
    struct putch_data pd;
    pd.buf = buf;
    pd.remaining = (int)size;

    if (size == 0)
        return 0;

    /* Amiga API requires old-style function pointer cast */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-prototypes"
    RawDoFmt((CONST_STRPTR)fmt, (APTR)ap, (void (*)())putch_func, &pd);
#pragma GCC diagnostic pop

    /* ensure NUL termination */
    if (pd.remaining > 0)
        *pd.buf = '\0';
    else if (size > 0)
        buf[size - 1] = '\0';

    return (int)(pd.buf - buf);
}

int snprintf(char *buf, unsigned long size, const char *fmt, ...);
int snprintf(char *buf, unsigned long size, const char *fmt, ...)
{
    va_list ap;
    int ret;

    va_start(ap, fmt);
    ret = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return ret;
}
