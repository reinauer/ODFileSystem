/*
 * libc_stubs.c — minimal C library stubs for handler builds
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The handler uses a custom entry point, so provide only the minimal
 * process-exit symbols it needs here and rely on libc for the rest.
 */

#include <exec/types.h>
#include "amiga_target_compat.h"
#include <proto/exec.h>

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
