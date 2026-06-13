/*
 * odfs/alloc.h — small allocation wrapper for shared code
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef ODFS_ALLOC_H
#define ODFS_ALLOC_H

#include "odfs/config.h"

#include <stddef.h>

#if ODFS_PLATFORM_AMIGA

#include "amiga_target_compat.h"

#include <exec/memory.h>
#include <exec/types.h>
#if ODFS_AMIGA_OS4
#include <exec/exectags.h>
#include <utility/tagitem.h>
#endif
#include <proto/exec.h>

static inline void *odfs_malloc(size_t size)
{
    if (size == 0)
        size = 1;
#if ODFS_AMIGA_OS4
    return AllocVecTags((ULONG)size, AVT_Type, MEMF_SHARED, TAG_END);
#else
    return AllocVec((ULONG)size, MEMF_PUBLIC);
#endif
}

static inline void *odfs_calloc(size_t count, size_t size)
{
    size_t total;

    if (size != 0 && count > ((size_t)-1) / size)
        return NULL;

    total = count * size;
    if (total == 0)
        total = 1;

#if ODFS_AMIGA_OS4
    return AllocVecTags((ULONG)total,
                        AVT_Type, MEMF_SHARED,
                        AVT_ClearWithValue, 0,
                        TAG_END);
#else
    return AllocVec((ULONG)total, MEMF_PUBLIC | MEMF_CLEAR);
#endif
}

static inline void odfs_free(void *ptr)
{
    if (ptr)
        FreeVec(ptr);
}

#else

#include <stdlib.h>

static inline void *odfs_malloc(size_t size)
{
    return malloc(size);
}

static inline void *odfs_calloc(size_t count, size_t size)
{
    return calloc(count, size);
}

static inline void odfs_free(void *ptr)
{
    free(ptr);
}

#endif

#endif /* ODFS_ALLOC_H */
