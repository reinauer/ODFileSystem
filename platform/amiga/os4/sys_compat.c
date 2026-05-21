/*
 * sys_compat.c - AmigaOS 4 integration helpers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "sys_compat.h"

#include <exec/exectags.h>
#include <exec/interfaces.h>
#include <interfaces/dos.h>
#include <interfaces/utility.h>
#include <utility/tagitem.h>
#include <utility/utility.h>

#include <proto/exec.h>
#include <proto/dos.h>

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;
struct UtilityBase *UtilityBase;

static struct DOSIFace *dos_iface;
static struct UtilityIFace *utility_iface;

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

    dos_iface = (struct DOSIFace *)GetInterface((struct Library *)DOSBase,
                                                (CONST_STRPTR)"main", 1,
                                                NULL);
    if (!dos_iface) {
        CloseLibrary((struct Library *)DOSBase);
        DOSBase = NULL;
        return 0;
    }
    IDOS = dos_iface;

    UtilityBase = (struct UtilityBase *)OpenLibrary(
        (CONST_STRPTR)"utility.library", 36);
    if (UtilityBase) {
        utility_iface = (struct UtilityIFace *)GetInterface(
            (struct Library *)UtilityBase, (CONST_STRPTR)"main", 1, NULL);
        if (!utility_iface) {
            CloseLibrary((struct Library *)UtilityBase);
            UtilityBase = NULL;
        }
    }

    return 1;
}

void odfs_amiga_close_libraries(void)
{
    if (utility_iface) {
        DropInterface((struct Interface *)utility_iface);
        utility_iface = NULL;
    }
    if (UtilityBase) {
        CloseLibrary((struct Library *)UtilityBase);
        UtilityBase = NULL;
    }
    if (dos_iface) {
        if (IDOS == dos_iface)
            IDOS = NULL;
        DropInterface((struct Interface *)dos_iface);
        dos_iface = NULL;
    }
    if (DOSBase) {
        CloseLibrary((struct Library *)DOSBase);
        DOSBase = NULL;
    }
}

void *odfs_amiga_alloc_vec(ULONG size, ULONG flags)
{
    if (size == 0)
        size = 1;

    if (flags & MEMF_CLEAR) {
        return AllocVecTags(size,
                            AVT_Type, flags & ~MEMF_CLEAR,
                            AVT_ClearWithValue, 0,
                            TAG_END);
    }

    return AllocVecTags(size, AVT_Type, flags, TAG_END);
}

void odfs_amiga_free_vec(void *ptr)
{
    if (ptr)
        FreeVec(ptr);
}

void *odfs_amiga_alloc_mem(ULONG size, ULONG flags)
{
    return odfs_amiga_alloc_vec(size, flags);
}

void odfs_amiga_free_mem(void *ptr, ULONG size)
{
    (void)size;
    odfs_amiga_free_vec(ptr);
}

struct MsgPort *odfs_amiga_create_msg_port(void)
{
    return AllocSysObjectTags(ASOT_PORT,
                              ASOPORT_AllocSig, TRUE,
                              ASOPORT_Action, PA_SIGNAL,
                              ASOPORT_Target, FindTask(NULL),
                              TAG_END);
}

void odfs_amiga_delete_msg_port(struct MsgPort *port)
{
    if (port)
        FreeSysObject(ASOT_PORT, port);
}

struct IORequest *odfs_amiga_create_io_request(struct MsgPort *port,
                                               ULONG size)
{
    return AllocSysObjectTags(ASOT_IOREQUEST,
                              ASOIOR_Size, size,
                              ASOIOR_ReplyPort, port,
                              TAG_END);
}

void odfs_amiga_delete_io_request(struct IORequest *req)
{
    if (req)
        FreeSysObject(ASOT_IOREQUEST, req);
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
    if (!utility_iface)
        return 1;

    return utility_iface->CallHookPkt(hook, object, message);
}
