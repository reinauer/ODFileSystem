/*
 * amiga_target_compat.h - AmigaOS 3 / AROS source compatibility
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef ODFS_AMIGA_TARGET_COMPAT_H
#define ODFS_AMIGA_TARGET_COMPAT_H

#define ODFS_AMIGA_OS4 0

#ifndef __NOLIBBASE__
#define __NOLIBBASE__
#endif

struct ExecBase;
#define SysBase (*(struct ExecBase **)4UL)

/*
 * OS4 V51+ shutdown packet. OS3 DOS never sends it, but accepting it
 * unconditionally keeps the shared packet loop free of OS conditionals.
 */
#ifndef ACTION_SHUTDOWN
#define ACTION_SHUTDOWN 3000
#endif

#endif /* ODFS_AMIGA_TARGET_COMPAT_H */
