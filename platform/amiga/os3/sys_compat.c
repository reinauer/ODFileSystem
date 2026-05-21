/*
 * sys_compat.c - AmigaOS 3 / AROS integration helpers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "sys_compat.h"

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/utility.h>

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;
struct Library *UtilityBase;

void odfs_amiga_init_sysbase(void)
{
    SysBase = *((struct ExecBase **)4L);
}

struct ExecBase *odfs_amiga_sysbase(void)
{
    return SysBase;
}

struct DosLibrary *odfs_amiga_dosbase(void)
{
    return DOSBase;
}

int odfs_amiga_open_libraries(void)
{
    DOSBase = (struct DosLibrary *)OpenLibrary((CONST_STRPTR)"dos.library",
                                               36);
    if (!DOSBase)
        return 0;

    UtilityBase = OpenLibrary((CONST_STRPTR)"utility.library", 36);
    return 1;
}

void odfs_amiga_close_libraries(void)
{
    if (UtilityBase) {
        CloseLibrary(UtilityBase);
        UtilityBase = NULL;
    }
    if (DOSBase) {
        CloseLibrary((struct Library *)DOSBase);
        DOSBase = NULL;
    }
}

void *odfs_amiga_alloc_mem(ULONG size, ULONG flags)
{
    return AllocMem(size, flags);
}

void odfs_amiga_free_mem(void *ptr, ULONG size)
{
    if (ptr)
        FreeMem(ptr, size);
}

void *odfs_amiga_alloc_vec(ULONG size, ULONG flags)
{
    return AllocVec(size, flags);
}

void odfs_amiga_free_vec(void *ptr)
{
    if (ptr)
        FreeVec(ptr);
}

struct MsgPort *odfs_amiga_create_msg_port(void)
{
    return CreateMsgPort();
}

void odfs_amiga_delete_msg_port(struct MsgPort *port)
{
    if (port)
        DeleteMsgPort(port);
}

struct IORequest *odfs_amiga_create_io_request(struct MsgPort *port,
                                               ULONG size)
{
    return CreateIORequest(port, size);
}

void odfs_amiga_delete_io_request(struct IORequest *req)
{
    if (req)
        DeleteIORequest(req);
}

LONG odfs_amiga_alloc_signal(LONG num)
{
    return AllocSignal(num);
}

void odfs_amiga_free_signal(LONG num)
{
    if (num != -1)
        FreeSignal(num);
}

void odfs_amiga_init_interrupt(struct Interrupt *intr,
                               const char *name,
                               APTR data,
                               odfs_amiga_interrupt_fn code)
{
    intr->is_Node.ln_Type = NT_INTERRUPT;
    intr->is_Node.ln_Pri = 0;
    intr->is_Node.ln_Name = (char *)name;
    intr->is_Data = data;
    intr->is_Code = (void (*)(void))(APTR)code;
}

ULONG odfs_amiga_call_hook_pkt(struct Hook *hook, APTR object, APTR message)
{
    if (!UtilityBase)
        return 1;

    return CallHookPkt(hook, object, message);
}
