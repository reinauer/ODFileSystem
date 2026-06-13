/*
 * start.c - freestanding AmigaOS 4 handler entry
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * A filesystem handler process receives an ACTION_STARTUP DosPacket as
 * its first process message. The newlib C runtime consumes the first
 * process message while detecting a Workbench start, which deadlocks
 * the mount, so the handler must not run any C runtime startup.
 *
 * The module supports two start paths:
 *
 * - As a normal disk-based handler, DOS enters the seglist at _start
 *   with the argument string in r3, its length in r4, and the ExecBase
 *   pointer in r5.
 *
 * - As a kickstart module, exec calls the Resident's rt_Init at
 *   coldstart (with ExecBase as the third argument); the init
 *   registers a FileSysEntry for DosType 'CD01' in FileSystem.resource
 *   whose fse_SegList is a fake seglist containing a 68k NOP+JMP gate
 *   to _start. The 68k gate does not carry the native register
 *   convention, so rt_Init stores ExecBase for _start to pick up.
 *   This mirrors the structure of the original CDFileSystem 53.4
 *   kickstart module.
 */

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/interfaces.h>
#include <exec/memory.h>
#include <exec/nodes.h>
#include <exec/resident.h>
#include <interfaces/exec.h>
#include <interfaces/dos.h>
#include <resources/filesysres.h>

#include <proto/exec.h>

#include "handler.h"
#include "sys_compat.h"

/* Interface globals normally provided by the C runtime startup. */
struct ExecIFace *IExec;
struct DOSIFace *IDOS;

int odfs_os4_handler_main(void);

int32 _start(STRPTR args, int32 arglen, struct ExecBase *sysbase);

int32 _start(STRPTR args, int32 arglen, struct ExecBase *sysbase)
{
    (void)args;
    (void)arglen;
    (void)sysbase;

    /*
     * The handler process is entered through the kickstart 68k seglist
     * gate (NOP + JMP), so register arguments are undefined here. Use
     * the ExecBase rt_Init stored; fall back to the legacy pointer at
     * absolute address 4 exactly like the original CDFileSystem module
     * (only relevant if the entry ever runs without the resident init).
     */
    if (!SysBase)
        SysBase = *((struct ExecBase **)4L);
    IExec = (struct ExecIFace *)SysBase->MainInterface;

#if ODFS_SERIAL_DEBUG
    DebugPrintF("[ODFS] _start: SysBase=%p IExec=%p\n", SysBase, IExec);
#endif
    return odfs_os4_handler_main();
}

/*
 * Fake seglist for the FileSysEntry. The BPTR points at .next; the
 * longword before it conventionally holds the segment size. The
 * "code" at BADDR+4 is a 68k NOP and JMP to the native _start; the
 * 68k emulator switches to native execution at the jump target.
 */
static const struct {
    uint32 size;
    uint32 next;
    uint16 nop;        /* m68k NOP */
    uint16 jmp;        /* m68k JMP absolute.l */
    int32 (*target)(STRPTR, int32, struct ExecBase *);
    uint32 endmark;
} odfs_ks_seg = {
    0x40,
    0,
    0x4e71,
    0x4ef9,
    _start,
    0xffffffff
};

static const char odfs_resident_name[] = "CDFileSystem";
static const char odfs_resident_id[] =
    "CDFileSystem 53.4 (ODFileSystem " ODFS_GIT_VERSION ")";

static APTR odfs_ks_init(APTR dummy1, APTR dummy2, struct ExecBase *sysbase)
{
    struct FileSysResource *fsr;
    struct FileSysEntry *fse;

    (void)dummy1;
    (void)dummy2;

    /* exec passes ExecBase as the third argument, like the original. */
    SysBase = sysbase;
    IExec = (struct ExecIFace *)SysBase->MainInterface;

#if ODFS_SERIAL_DEBUG
    DebugPrintF("[ODFS] ks_init: SysBase=%p\n", SysBase);
#endif

    fsr = OpenResource((CONST_STRPTR)FSRNAME);
#if ODFS_SERIAL_DEBUG
    DebugPrintF("[ODFS] ks_init: fsr=%p\n", fsr);
#endif
    if (!fsr)
        return NULL;

    /*
     * Mirror the original module's plain AllocMem: AllocVecTags is not
     * a safe assumption this early in coldstart, and the entry is
     * never freed.
     */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    fse = AllocMem(sizeof(*fse), MEMF_PUBLIC | MEMF_CLEAR);
#pragma GCC diagnostic pop
#if ODFS_SERIAL_DEBUG
    DebugPrintF("[ODFS] ks_init: fse=%p\n", fse);
#endif
    if (!fse)
        return NULL;

    fse->fse_Node.ln_Name = (char *)odfs_resident_id;
    fse->fse_DosType = 0x43443031; /* CD01 */
    fse->fse_Version = (54UL << 16);
    fse->fse_PatchFlags = FSEF_SEGLIST | FSEF_GLOBVEC | FSEF_STACKSIZE;
    fse->fse_StackSize = 16384;
    fse->fse_Priority = 10;
    fse->fse_SegList = MKBADDR(&odfs_ks_seg.next);
    fse->fse_GlobalVec = -1;

    AddHead(&fsr->fsr_FileSysEntries, &fse->fse_Node);
#if ODFS_SERIAL_DEBUG
    DebugPrintF("[ODFS] ks_init: CD01 entry registered, seg=%p\n",
                (APTR)fse->fse_SegList);
#endif
    return fse;
}

/*
 * Kickstart filesystem modules announce themselves with a Resident
 * structure; exec runs rt_Init at coldstart, which registers the
 * filesystem. Field values mirror the original CDFileSystem 53.4
 * resident (RTF_NATIVE | RTF_COLDSTART, priority 79).
 */
extern const struct Resident odfs_os4_resident;

const struct Resident odfs_os4_resident = {
    RTC_MATCHWORD,
    (struct Resident *)&odfs_os4_resident,
    (APTR)(&odfs_os4_resident + 1),
    RTF_NATIVE | RTF_COLDSTART,
    53,
    NT_UNKNOWN,
    79,
    (char *)odfs_resident_name,
    (char *)odfs_resident_id,
    (APTR)odfs_ks_init
};
