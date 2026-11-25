/*
 * system_charset.c - the system's character set, from codesets.library
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * This is in a file of its own, listed ahead of handler_main.c in the
 * Makefile, to keep it out of the way: the linker places handler_main.o
 * and the objects after it in front of the core, so code added there
 * moves every routine of the core and the backends.
 */

#include "handler.h"

#if ODFS_FEATURE_CHARSET_EXTERNAL

#include <libraries/codesets.h>

#include <proto/exec.h>
#include <proto/dos.h>
#if ODFS_AMIGA_OS4
/* the interface is a local variable here, not a global */
#define __NOGLOBALIFACE__
#endif
#include <proto/codesets.h>

#if !ODFS_AMIGA_OS4
#define DOSBase (g->libs.dos)
#endif

#include <string.h>

#include "odfs/charset.h"
#include "odfs/log.h"

/*
 * Nonzero if every directory of assign name is on another handler's
 * volume, so that loading a file through it asks nothing of this one.
 * A late or non-binding assign shows where it leads only once it is
 * used; that and a missing assign count as no. dl is the locked list.
 */
static int assign_is_elsewhere(const handler_global_t *g, struct DosList *dl,
                               const char *name)
{
    const struct AssignList *al;
    BPTR lock;

    dl = FindDosEntry(dl, (CONST_STRPTR)name, LDF_ASSIGNS);
    if (!dl || dl->dol_Type != DLT_DIRECTORY)
        return 0;

    lock = dl->dol_Lock;
    al = dl->dol_misc.dol_assign.dol_List;
    for (;;) {
        if (!lock ||
            ((struct FileLock *)BADDR(lock))->fl_Task == g->dosport)
            return 0;
        if (!al)
            return 1;
        lock = al->al_Lock;
        al = al->al_Next;
    }
}

/*
 * Whether the volume name is plain ASCII on the disc, which reads the
 * same in every character set. The backend is asked, since in the
 * mount's copy a '_' may be one from the disc or stand for a letter
 * Latin-1 does not have.
 */
static int volume_name_is_ascii(handler_global_t *g)
{
    char name[sizeof(g->mount.volume_name)];
    const unsigned char *p = (const unsigned char *)name;

    if (!g->mount.backend_ops || !g->mount.backend_ops->get_volume_name ||
        g->mount.backend_ops->get_volume_name(g->mount.backend_ctx, name,
                                              sizeof(name)) != ODFS_OK)
        return 0;
    while (*p && *p < 0x80)
        p++;
    return *p == '\0';
}

/*
 * Copy the system's default character set out of codesets.library, in
 * the form odfs/charset.h describes. Fails if the library is not
 * installed or the set is not an 8-bit one built on ASCII.
 */
static int read_codeset(uint16_t *map)
{
    struct Process *proc = (struct Process *)FindTask(NULL);
    APTR window = proc->pr_WindowPtr;
    struct Library *CodesetsBase;
#if ODFS_AMIGA_OS4
    struct CodesetsIFace *ICodesets = NULL;
#endif
    int ok = 0;

    /* a volume that is not there is a failure, not a requester */
    proc->pr_WindowPtr = (APTR)-1;
    CodesetsBase = OpenLibrary((CONST_STRPTR)CODESETSNAME, CODESETSVER);
#if ODFS_AMIGA_OS4
    if (CodesetsBase)
        ICodesets = (struct CodesetsIFace *)
            GetInterface(CodesetsBase, "main", 1, NULL);
    if (ICodesets) {
#else
    if (CodesetsBase) {
#endif
        const struct codeset *cs = CodesetsFindA(NULL, NULL);
        unsigned int i;

        /* "UTF-8" is in the list too, with an empty table */
        ok = cs != NULL;
        for (i = 0x20; ok && i < 0x7F; i++)
            ok = cs->table[i].ucs4 == i;
        for (i = 0; ok && i < ODFS_CHARSET_MAP_SIZE; i++) {
            ULONG ch = cs->table[ODFS_CHARSET_MAP_FIRST + i].ucs4;

            /* nothing below U+00A0: those are control codes again */
            map[i] = (ch >= 0xA0 && ch <= 0xFFFF) ? (uint16_t)ch : 0;
        }
#if ODFS_AMIGA_OS4
        DropInterface((struct Interface *)ICodesets);
#endif
    }
    if (CodesetsBase) {
        /*
         * The library and its tables take some 300 KB. Have it leave
         * memory again when its last user closes it, which is now
         * unless a program has it open.
         */
        Forbid();
        RemLibrary(CodesetsBase);
        CloseLibrary(CodesetsBase);
        Permit();
    }
    proc->pr_WindowPtr = window;

    return ok;
}

/*
 * The character set is asked for at the first request that concerns
 * the disc. This is not done at startup: a handler that Mount starts
 * from DEVS:DOSDrivers runs before the locale preferences are loaded,
 * and codesets.library keeps the default it works out when it is first
 * opened, for every program that opens it later.
 *
 * On AmigaOS 3 and AROS the packet loop must not notice. A test per
 * packet there costs time that CDBench shows, and so does any other
 * change to that function: the compiler then lays the whole loop out
 * anew. So:
 *
 *  - mount_volume() calls odfs_amiga_system_charset() every time. The
 *    first call, at startup, turns the packet port into one that runs
 *    a software interrupt.
 *  - The interrupt runs for every packet until one asks about the
 *    disc. Then it turns the port back, and makes the handler believe
 *    that the disc may have been changed, which mounts it again before
 *    the packet is taken.
 *  - unmount_volume() calls odfs_amiga_system_charset() too, and that
 *    call asks. If the disc is the same and its volume name does not
 *    depend on the answer, it tells unmount_volume() to leave the disc
 *    mounted.
 *
 * On AmigaOS 4 requests are calls in the caller's context. The first
 * one, odfs_amiga_first_call(), sends the handler process down the
 * same path and waits for it.
 */

/* the handler acts as if the disc may have been changed */
static void force_media_pass(handler_global_t *g)
{
    g->names_state = ODFS_NAMES_DUE;
    g->names_count = g->change_count;
    g->names_counted = (uint8_t)(g->change_count_valid != 0);
    /* handle_media_change() mounts again if it cannot tell */
    g->change_count_valid = 0;
}

#if ODFS_AMIGA_OS4

void odfs_amiga_first_call(handler_global_t *g)
{
    struct Task *me = FindTask(NULL);

    if (me == g->handler_task || g->inhibited || g->names_waiter)
        return;

    ODFS_INFO(&g->log, ODFS_SUB_CORE, "names: first call, from %s",
              me->tc_Node.ln_Name ? me->tc_Node.ln_Name : "?");
    force_media_pass(g);
    g->names_waiter = me;
    SetSignal(0, SIGF_SINGLE);
    ReleaseSemaphore(&g->fs_sem);
    Signal(g->handler_task, (ULONG)1 << g->chgsigbit);
    Wait(SIGF_SINGLE);
    ObtainSemaphore(&g->fs_sem);
}

void odfs_amiga_media_pass_done(handler_global_t *g)
{
    struct Task *waiter = g->names_waiter;

    if (waiter) {
        g->names_waiter = NULL;
        Signal(waiter, SIGF_SINGLE);
    }
}

#else /* !ODFS_AMIGA_OS4 */

static LONG first_packet(APTR data)
{
    handler_global_t *g = data;
    struct MsgPort *port = g->dosport;
    ULONG sigs = (ULONG)1 << port->mp_SigBit;
    const struct Node *node;

    if (g->names_state != ODFS_NAMES_AWAITED)
        return 0;

    /*
     * ACTION_IS_FILESYSTEM asks nothing about the disc, so it does not
     * count. Workbench sends it first, with the DOS list locked, and
     * the disc's volume node cannot be taken out of the list then.
     */
    for (node = port->mp_MsgList.lh_Head; node->ln_Succ;
         node = node->ln_Succ) {
        const struct DosPacket *pkt =
            (const struct DosPacket *)(uintptr_t)node->ln_Name;

        if (pkt->dp_Type != ACTION_IS_FILESYSTEM) {
            force_media_pass(g);
            g->names_msg = node;
            port->mp_SigTask = g->names_task;
            port->mp_Flags = (UBYTE)((port->mp_Flags & ~PF_ACTION) |
                                     PA_SIGNAL);
            sigs |= (ULONG)1 << g->chgsigbit;
            break;
        }
    }
    Signal(g->names_task, sigs);
    return 0;
}

#endif /* !ODFS_AMIGA_OS4 */

/*
 * Is the system's loader waiting for this handler, and is the request
 * that asked for this pass still waiting? If that one has been taken,
 * this is not the pass it asked for, and an unmount is somebody's wish.
 */
static int loader_is_waiting(handler_global_t *g, int *pending)
{
    int loader = 0;
#if ODFS_AMIGA_OS4
    const struct Task *waiter = g->names_waiter;

    *pending = waiter != NULL;
    if (waiter && waiter->tc_Node.ln_Name &&
        strcmp(waiter->tc_Node.ln_Name, ODFS_AMIGA_LOADER_TASK) == 0)
        loader = 1;
#else
    const struct Node *node;

    *pending = 0;
    Forbid();
    for (node = g->dosport->mp_MsgList.lh_Head; node->ln_Succ;
         node = node->ln_Succ) {
        const struct MsgPort *reply =
            ((const struct DosPacket *)(uintptr_t)node->ln_Name)->dp_Port;

        if (node == g->names_msg)
            *pending = 1;
        if (reply && (reply->mp_Flags & PF_ACTION) == PA_SIGNAL) {
            const struct Task *waiter = reply->mp_SigTask;

            if (waiter && waiter->tc_Node.ln_Name &&
                strcmp(waiter->tc_Node.ln_Name,
                       ODFS_AMIGA_LOADER_TASK) == 0)
                loader = 1;
        }
    }
    Permit();
#endif
    return loader;
}

/*
 * Find out which character set the system uses, so that names are
 * converted to it instead of to Latin-1. Returns nonzero if the disc
 * can stay mounted: see above.
 *
 * The library comes from disk, and the handler waits here while the
 * system's loader process fetches it. The loader must not in turn wait
 * for this handler. So names stay in Latin-1 if a request of the
 * loader's own is waiting, or if a directory the loader searches is on
 * this handler's volume or cannot be told: the boot volume, LIBS:, or
 * DEVS:, from where a request the loader has queued may be loading a
 * device.
 */
int odfs_amiga_system_charset(handler_global_t *g)
{
    struct DosList *dl;
    int safe;
    int same;

    if (g->names_state == ODFS_NAMES_UNASKED) {
        /* without a media change signal there is no way back here */
        g->names_state = ODFS_NAMES_DONE;
        if (g->chgsigbit < 0)
            return 0;

#if ODFS_AMIGA_OS4
        g->names_state = ODFS_NAMES_AWAITED;
#else
        {
            struct MsgPort *port = g->dosport;

            g->names_task = port->mp_SigTask;
            odfs_amiga_init_interrupt(&g->names_int, "odfs-firstpacket",
                                      g, first_packet);
            Disable();
            if (!port->mp_MsgList.lh_Head->ln_Succ) {
                g->names_state = ODFS_NAMES_AWAITED;
                port->mp_SoftInt = &g->names_int.intr;
                port->mp_Flags = (UBYTE)((port->mp_Flags & ~PF_ACTION) |
                                         PA_SOFTINT);
            }
            Enable();
        }
#endif
        /* else a packet is here already: ask now */
        if (g->names_state == ODFS_NAMES_AWAITED)
            return 0;
    } else if (g->names_state != ODFS_NAMES_DUE) {
        return 0;
    }
    g->names_state = ODFS_NAMES_DONE;

    safe = !loader_is_waiting(g, &same);
    if (safe) {
        /*
         * Not AttemptLockDosList(): it fails while anyone else has
         * the list, even for reading as Workbench does around its
         * first packet. Waiting for a read lock is what the loader
         * does too.
         */
        dl = LockDosList(LDF_ASSIGNS | LDF_READ);
        safe = assign_is_elsewhere(g, dl, "SYS") &&
               assign_is_elsewhere(g, dl, "LIBS") &&
               assign_is_elsewhere(g, dl, "DEVS");
        UnLockDosList(LDF_ASSIGNS | LDF_READ);
    }

    /* a different disc than when the pass was asked for is mounted anew */
    if (!g->names_counted || !g->change_count_valid ||
        g->change_count != g->names_count)
        same = 0;

    if (safe && read_codeset(g->name_map)) {
        ODFS_INFO(&g->log, ODFS_SUB_CORE,
                  "names follow the system's character set");
        g->names_mapped = 1;

        /*
         * The disc was mounted for Latin-1, but none of its names has
         * been handed out. If its volume name is plain ASCII, switching
         * the set is all it takes.
         */
        if (same && (!g->mounted ||
                     g->mount.cache.names.charset != ODFS_NAMES_LATIN1 ||
                     !volume_name_is_ascii(g)))
            same = 0;
        if (same) {
            g->mount.opts.name_charset = ODFS_NAMES_MAPPED;
            g->mount.opts.name_map = g->name_map;
            g->mount.cache.names.charset = ODFS_NAMES_MAPPED;
            g->mount.cache.names.map = g->name_map;
        }
    } else {
        ODFS_INFO(&g->log, ODFS_SUB_CORE, "names stay Latin-1%s",
                  safe ? "" : ": not safe to load a library now");
    }

    return same;
}

#endif /* ODFS_FEATURE_CHARSET_EXTERNAL */
