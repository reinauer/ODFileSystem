/*
 * aros_compat.h — AROS compatibility layer
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Provides macros that work on both classic AmigaOS (m68k-amigaos-gcc)
 * and AROS (m68k-aros-gcc or i386-aros-gcc). On AROS, BSTR/BPTR
 * handling may differ depending on AROS_FAST_BSTR/AROS_FAST_BPTR.
 */

#ifndef ODFS_AROS_COMPAT_H
#define ODFS_AROS_COMPAT_H

#ifdef __AROS__

#include <aros/macros.h>
#include <dos/bptr.h>

/* AROS provides these in dos/bptr.h */
#ifndef AROS_BSTR_ADDR
#define AROS_BSTR_ADDR(s) (((STRPTR)BADDR(s))+1)
#endif
#ifndef AROS_BSTR_strlen
#define AROS_BSTR_strlen(s) (AROS_BSTR_ADDR(s)[-1])
#endif

/* IPTR is defined by AROS in exec/types.h */

#else /* classic AmigaOS */

/* Classic AmigaOS: BSTR is a BPTR to a BCPL string (length-prefixed) */
#define AROS_BSTR_ADDR(s)    (((UBYTE *)BADDR(s)) + 1)
#define AROS_BSTR_strlen(s)  (((UBYTE *)BADDR(s))[0])

/* IPTR: integer type wide enough to hold a pointer */
#ifndef IPTR
typedef unsigned long IPTR;
#endif
#ifndef SIPTR
typedef long SIPTR;
#endif

#endif /* __AROS__ */

#endif /* ODFS_AROS_COMPAT_H */
