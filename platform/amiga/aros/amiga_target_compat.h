/*
 * amiga_target_compat.h - AROS integration
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
extern struct ExecBase *SysBase;

#ifndef ACTION_SHUTDOWN
#define ACTION_SHUTDOWN 3000
#endif

#endif /* ODFS_AMIGA_TARGET_COMPAT_H */
