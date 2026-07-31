/*
 * sys_compat.c - AmigaOS 3 / AROS integration helpers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "sys_compat.h"

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/utility.h>

#include <string.h>

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;
struct Library *UtilityBase;

static odfs_amiga_interrupt_fn interrupt_code;

/*
 * Handler instances started from one seglist (FileSysResource or a
 * resident L:ODFileSystem) share this data segment, so the library
 * bases above are shared as well. Reference-count the openers under
 * Forbid() and close only when the last instance exits; otherwise the
 * first shutdown — including a declined second mount of the same
 * device — NULLs the bases out from under every surviving instance.
 */
static LONG lib_users;

static LONG odfs_amiga_interrupt_entry(APTR data asm("a1"))
{
    return interrupt_code ? interrupt_code(data) : 0;
}

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
    int ok = 1;

    /* dos and utility are ROM-resident on V37+, so OpenLibrary cannot
     * Wait() here and the Forbid() holds across the opens. */
    Forbid();
    if (lib_users == 0) {
        /*
         * V37 (Kickstart 2.04) is the real floor: the ExAll path calls
         * MatchPatternNoCase() and startup.S may call StackSwap(), both
         * V37. Requiring V37 here turns a latent crash on the
         * short-lived 2.00 ROMs into a clean load failure.
         */
        DOSBase = (struct DosLibrary *)
            OpenLibrary((CONST_STRPTR)"dos.library", 37);
        if (DOSBase)
            UtilityBase = OpenLibrary((CONST_STRPTR)"utility.library", 36);
        else
            ok = 0;
    }
    if (ok)
        lib_users++;
    Permit();

    return ok;
}

void odfs_amiga_close_libraries(void)
{
    Forbid();
    if (lib_users > 0 && --lib_users == 0) {
        if (UtilityBase) {
            CloseLibrary(UtilityBase);
            UtilityBase = NULL;
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
    namebuf[0] = (UBYTE)namelen;
    memcpy(namebuf + 1, name, namelen);

    dl->dol_Type = type;
    dl->dol_Name = MKBADDR(namebuf);
    return dl;
}

void odfs_amiga_delete_dos_entry(void *node)
{
    if (node)
        FreeMem(node, sizeof(struct DosList) + 32u);
}

void odfs_amiga_init_interrupt(struct Interrupt *intr,
                               const char *name,
                               APTR data,
                               odfs_amiga_interrupt_fn code)
{
    interrupt_code = code;
    intr->is_Node.ln_Type = NT_INTERRUPT;
    intr->is_Node.ln_Pri = 0;
    intr->is_Node.ln_Name = (char *)name;
    intr->is_Data = data;
    intr->is_Code = (void (*)(void))(APTR)odfs_amiga_interrupt_entry;
}

ULONG odfs_amiga_call_hook_pkt(struct Hook *hook, APTR object, APTR message)
{
    if (!UtilityBase)
        return 1;

    return CallHookPkt(hook, object, message);
}
