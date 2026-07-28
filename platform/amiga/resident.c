/*
 * resident.c
 *
 * Resident (romtag) initialisation for the m68k ODFileSystem handler.
 *
 * On coldstart the OS scans the binary for this romtag and calls rt_init,
 * which registers ODFileSystem with FileSystem.resource by adding a
 * FileSysEntry for our DosType. This lets DOS mount and (re)load the
 * filesystem from the resource -- required when the handler lives in a
 * custom ROM, and harmless when loaded from disk. The entry is only added
 * if one for the same DosType isn't already present.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 */

#include <proto/exec.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/resident.h>
#include <exec/nodes.h>
#include <exec/lists.h>
#include <resources/filesysres.h>

#define DOSTYPE 0x43443031

#ifndef ODFS_VERSION_MAJOR
#define ODFS_VERSION_MAJOR 0
#endif

#ifndef ODFS_VERSION_MINOR
#define ODFS_VERSION_MINOR 0
#endif

extern const char version_string[];
static const char name_string[] = "ODFileSystem";
static const char creator_string[] =
    "ODFileSystem " ODFS_GIT_VERSION " (" ODFS_AMIGA_DATE ")";

void entrypoint(void);
static int rt_init(BPTR seglist asm("a0"), struct ExecBase *SysBase asm("a6"));

static const struct Resident __attribute__((used,no_reorder)) romtag = {
    .rt_MatchWord = RTC_MATCHWORD,
    .rt_MatchTag  = (APTR)&romtag,
    .rt_EndSkip   = (APTR)(&romtag+1),
    .rt_Flags     = RTF_COLDSTART,
    .rt_Version   = ODFS_VERSION_MAJOR,
    .rt_Type      = NT_UNKNOWN,
    .rt_Pri       = 10,
    .rt_Name      = (APTR)&name_string,
    .rt_IdString  = (APTR)&version_string,
    .rt_Init      = (APTR)rt_init
};

static int rt_init(BPTR seglist asm("a0"), struct ExecBase *SysBase asm("a6")) {
    struct FileSysResource *fsr = OpenResource((STRPTR)FSRNAME);
    struct FileSysEntry *fse = NULL;
    BOOL found = FALSE;

    if (fsr) {
        /* Check if there's already a CDFS in FileSystem.resource */
        Forbid();
        for (fse = (struct FileSysEntry*)fsr->fsr_FileSysEntries.lh_Head;
             fse->fse_Node.ln_Succ != NULL;
             fse = (struct FileSysEntry*)fse->fse_Node.ln_Succ)
        {
            if (fse->fse_DosType == DOSTYPE) {
                /* Already one there, don't bother adding this */
                found = TRUE;
                break;
            }
        }
        Permit();

        if (!found) {
            fse = AllocMem(sizeof(struct FileSysEntry), MEMF_PUBLIC | MEMF_CLEAR);
            if (fse) {
                // seglist will be NULL if this was part of a custom ROM
                // Back up from the entrypoint to fake the seglist
                if (seglist == NULL) {
                    seglist = MKBADDR((UBYTE *)entrypoint - 4);
                }
                fse->fse_Node.ln_Name = (char *)creator_string;
                fse->fse_DosType      = DOSTYPE;
                fse->fse_GlobalVec    = -1;
                fse->fse_PatchFlags   = 0x190;
                fse->fse_StackSize    = 16384;
                fse->fse_Version      =
                    ODFS_VERSION_MAJOR << 16 | ODFS_VERSION_MINOR;
                fse->fse_SegList      = (BPTR)seglist;
                fse->fse_Priority     = 10;

                Forbid();
                AddHead(&fsr->fsr_FileSysEntries,(struct Node *)fse);
                Permit();
            }
        }

    }

    return 0;
}
