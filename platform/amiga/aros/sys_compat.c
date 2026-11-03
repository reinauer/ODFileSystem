/*
 * sys_compat.c - AROS integration helpers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "amiga_target_compat.h"
#include "sys_compat.h"
#include "aros_compat.h"

#include <clib/alib_protos.h>
#include <proto/exec.h>

#include <string.h>


static AROS_INTH1(odfs_amiga_interrupt_entry, odfs_amiga_interrupt_t *, ai)
{
    AROS_INTFUNC_INIT

    return (ai && ai->fn) ? (ULONG)ai->fn(ai->data) : 0;

    AROS_INTFUNC_EXIT
}

int odfs_amiga_open_libraries(odfs_amiga_libs_t *libs)
{
    /* The shared ExAll path uses the V37 MatchPatternNoCase API. */
    libs->dos = (struct DosLibrary *)OpenLibrary((CONST_STRPTR)"dos.library", 37);
    return libs->dos != NULL;
}

void odfs_amiga_close_libraries(odfs_amiga_libs_t *libs)
{
    if (libs->dos) {
        CloseLibrary((struct Library *)libs->dos);
        libs->dos = NULL;
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
    return AllocSignal((BYTE)num);
}

void odfs_amiga_free_signal(LONG num)
{
    if (num != -1)
        FreeSignal((BYTE)num);
}

void *odfs_amiga_create_dos_entry(const char *name, LONG type)
{
    struct DosList *dl;
    UBYTE *namebuf;
    size_t namelen;

    if (!name)
        return NULL;

    namelen = strlen(name);
    if (namelen > 30)
        namelen = 30;

    /*
     * Build the DosList entry directly instead of routing the name
     * through MakeDosEntry(), which may normalize names containing
     * AmigaDOS metacharacters such as parentheses.
     */
    dl = AllocMem(sizeof(*dl) + 32u, MEMF_PUBLIC | MEMF_CLEAR);
    if (!dl)
        return NULL;

    namebuf = (UBYTE *)(dl + 1);
    dl->dol_Name = (BSTR)MKBADDR(namebuf);
    AROS_BSTR_setstrlen(dl->dol_Name, (UBYTE)namelen);
    memcpy(AROS_BSTR_ADDR(dl->dol_Name), name, namelen);

    dl->dol_Type = type;
    return dl;
}

void odfs_amiga_delete_dos_entry(void *node)
{
    if (node)
        FreeMem(node, sizeof(struct DosList) + 32u);
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
    ai->intr.is_Data = ai;
    ai->intr.is_Code = (void (*)(void))(APTR)odfs_amiga_interrupt_entry;
}

ULONG odfs_amiga_call_hook_pkt(struct Hook *hook, APTR object, APTR message)
{
    return (ULONG)CallHookA(hook, object, message);
}
