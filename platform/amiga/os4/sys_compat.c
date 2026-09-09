/*
 * sys_compat.c - AmigaOS 4 integration helpers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "sys_compat.h"

#include <dos/dostags.h>
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

/*
 * Handler instances started from one seglist (the kickstart module's
 * FileSysEntry or a reused disk-based one) share this data segment, so
 * the bases and interfaces above — including IDOS — are shared as
 * well. Reference-count the openers under Forbid() and close only when
 * the last instance exits; otherwise the first shutdown — including a
 * declined second mount of the same device — NULLs them out from under
 * every surviving instance.
 */
static LONG lib_users;

/*
 * V50+ interrupt entry. Soft interrupts fired through Cause() receive
 * (0, SysBase, is_Data); interrupt servers receive (context, SysBase,
 * userData). Both pass is_Data third, so one entry covers either path.
 */
static void odfs_amiga_interrupt_entry(int32 unused,
                                       struct ExecBase *sysbase,
                                       APTR data)
{
    odfs_amiga_interrupt_t *ai = data;

    (void)unused;
    (void)sysbase;

    if (ai && ai->fn)
        ai->fn(ai->data);
}

static int open_libraries_first(void)
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

int odfs_amiga_open_libraries(odfs_amiga_libs_t *libs)
{
    int ok = 1;

    /* dos and utility are kickstart-resident, so the opens cannot
     * Wait() and the Forbid() holds across them. */
    Forbid();
    if (lib_users == 0)
        ok = open_libraries_first();
    if (ok)
        lib_users++;
    Permit();

    libs->dos = ok ? DOSBase : NULL;
    return ok;
}

void odfs_amiga_close_libraries(odfs_amiga_libs_t *libs)
{
    /* the instance drops its copy; the bases themselves are refcounted here */
    libs->dos = NULL;

    Forbid();
    if (lib_users > 0 && --lib_users == 0) {
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
    Permit();
}

void *odfs_amiga_alloc_mem(ULONG size, ULONG flags)
{
    ULONG type;

    if (size == 0)
        size = 1;

    /*
     * AVT_Type only accepts MEMF_PRIVATE, MEMF_SHARED, and
     * MEMF_EXECUTABLE. Translate the legacy flags the shared handler
     * uses (MEMF_PUBLIC, de_BufMemType bits): handler memory is shared
     * with DOS and other processes, so legacy MEMF_PUBLIC maps to
     * MEMF_SHARED.
     */
    type = (flags & MEMF_PRIVATE) ? MEMF_PRIVATE : MEMF_SHARED;
    if (flags & MEMF_EXECUTABLE)
        type |= MEMF_EXECUTABLE;

    if (flags & MEMF_CLEAR) {
        return AllocVecTags(size,
                            AVT_Type, type,
                            AVT_ClearWithValue, 0,
                            TAG_END);
    }

    return AllocVecTags(size, AVT_Type, type, TAG_END);
}

void odfs_amiga_free_mem(void *ptr, ULONG size)
{
    (void)size;
    if (ptr)
        FreeVec(ptr);
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
    return AllocSignal((BYTE)num);
}

void odfs_amiga_free_signal(LONG num)
{
    if (num != -1)
        FreeSignal((BYTE)num);
}

void *odfs_amiga_create_dos_entry(const char *name, LONG type)
{
    if (!name)
        return NULL;

    return AllocDosObjectTags(DOS_DOSLIST,
                              ADO_Type, (ULONG)type,
                              ADO_Name, (ULONG)name,
                              TAG_END);
}

void odfs_amiga_delete_dos_entry(void *node)
{
    if (node)
        FreeDosObject(DOS_DOSLIST, node);
}

void odfs_amiga_init_interrupt(odfs_amiga_interrupt_t *ai,
                               const char *name,
                               APTR data,
                               odfs_amiga_interrupt_fn code)
{
    ai->fn   = code;
    ai->data = data;
    ai->intr.is_Node.ln_Type = NT_INTERRUPT;
    ai->intr.is_Node.ln_Pri = 0;
    ai->intr.is_Node.ln_Name = (char *)name;
    /* is_Data is the wrapper, so the trampoline reaches this instance's
       callback without a global to look it up in */
    ai->intr.is_Data = ai;
    ai->intr.is_Code = (void (*)(void))(APTR)odfs_amiga_interrupt_entry;
}

ULONG odfs_amiga_call_hook_pkt(struct Hook *hook, APTR object, APTR message)
{
    if (!utility_iface)
        return 1;

    return utility_iface->CallHookPkt(hook, object, message);
}
