/*
 * start.c - AROS handler entry without C runtime startup
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "handler.h"

#include <aros/asmcall.h>

struct ExecBase *SysBase;

/* Also used as handler_func by AROS's generated resident module. */
LONG odfs_aros_handler(struct ExecBase *sysbase);

LONG odfs_aros_handler(struct ExecBase *sysbase)
{
    SysBase = sysbase;
    handler_main();
    return RETURN_OK;
}

/* genmodule supplies its own entry for the in-tree resident build. */
#ifndef ODFS_AROS_RESIDENT
__startup static AROS_PROCH(odfs_aros_start, argstr, argsize, sysbase)
{
    AROS_PROCFUNC_INIT

    (void)argstr;
    (void)argsize;
    return odfs_aros_handler(sysbase);

    AROS_PROCFUNC_EXIT
}
#endif
