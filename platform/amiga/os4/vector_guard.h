/*
 * Serialize OS4 vector entry with handler shutdown.
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef ODFS_OS4_VECTOR_GUARD_H
#define ODFS_OS4_VECTOR_GUARD_H

#include "handler.h"
#include <proto/exec.h>

static inline handler_global_t *odfs_os4_acquire_handler(
    struct FileSystemVectorPort *vp)
{
    handler_global_t *g;

    /* A waiter must join the semaphore before shutdown can detach g. */
    Forbid();
    g = vp ? (handler_global_t *)vp->FSV.FSPrivate : NULL;
    if (g)
        ObtainSemaphore(&g->fs_sem);
    Permit();
    return g;
}

static inline int odfs_os4_begin_shutdown(handler_global_t *g)
{
    int idle;

    /* Never wait: an active or queued callback may still create objects. */
    Forbid();
    if (!AttemptSemaphore(&g->fs_sem)) {
        Permit();
        return 0;
    }

    idle = g->locklist.mlh_TailPred ==
               (struct MinNode *)&g->locklist.mlh_Head &&
           g->fhlist.mlh_TailPred ==
               (struct MinNode *)&g->fhlist.mlh_Head;
    if (idle && g->vector_port) {
        g->vector_port->FSV.Version = 0;
        g->vector_port->FSV.FSPrivate = NULL;
    }
    ReleaseSemaphore(&g->fs_sem);
    Permit();
    return idle;
}

#endif
