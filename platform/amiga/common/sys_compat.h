/*
 * sys_compat.h - Amiga-family OS integration boundary
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef ODFS_AMIGA_SYS_COMPAT_H
#define ODFS_AMIGA_SYS_COMPAT_H

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/io.h>
#include <exec/interrupts.h>
#include <exec/libraries.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/filehandler.h>

#include <stddef.h>

struct Hook;
struct FileInfoBlock;
struct DeviceNode;

typedef LONG (*odfs_amiga_interrupt_fn)(APTR data);

extern struct ExecBase *SysBase;
extern struct DosLibrary *DOSBase;

void odfs_amiga_init_sysbase(void);
struct ExecBase *odfs_amiga_sysbase(void);
struct DosLibrary *odfs_amiga_dosbase(void);

int odfs_amiga_open_libraries(void);
void odfs_amiga_close_libraries(void);

void *odfs_amiga_alloc_mem(ULONG size, ULONG flags);
void odfs_amiga_free_mem(void *ptr, ULONG size);
void *odfs_amiga_alloc_vec(ULONG size, ULONG flags);
void odfs_amiga_free_vec(void *ptr);

struct MsgPort *odfs_amiga_create_msg_port(void);
void odfs_amiga_delete_msg_port(struct MsgPort *port);
struct IORequest *odfs_amiga_create_io_request(struct MsgPort *port,
                                               ULONG size);
void odfs_amiga_delete_io_request(struct IORequest *req);

LONG odfs_amiga_alloc_signal(LONG num);
void odfs_amiga_free_signal(LONG num);

void odfs_amiga_init_interrupt(struct Interrupt *intr,
                               const char *name,
                               APTR data,
                               odfs_amiga_interrupt_fn code);

void odfs_amiga_set_fib_entry_type(struct FileInfoBlock *fib, LONG type);
void odfs_amiga_copy_device_lock(struct DeviceNode *dst,
                                 const struct DeviceNode *src);
ULONG odfs_amiga_call_hook_pkt(struct Hook *hook, APTR object, APTR message);

#endif /* ODFS_AMIGA_SYS_COMPAT_H */
