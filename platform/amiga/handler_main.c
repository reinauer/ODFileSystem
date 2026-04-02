/*
 * handler_main.c — AmigaDOS packet handler for ODFileSystem
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * This is the AmigaDOS filesystem handler. It receives DOS packets,
 * dispatches them to the appropriate handlers, and returns results.
 * Read-only — all write operations return ERROR_DISK_WRITE_PROTECTED.
 */

#include "handler.h"

#if ODFS_FEATURE_CDDA
#include "cdda/cdda.h"
#endif

#include <exec/execbase.h>
#include <exec/io.h>
#include <devices/trackdisk.h>
#include <devices/scsidisk.h>
#include <devices/input.h>
#include <dos/dostags.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include <string.h>

static const char version_string[] =
    "$VER: ODFileSystem " ODFS_HANDLER_VERSION " (" __DATE__ ")";

/* library bases — set by handler_main() */
struct ExecBase *SysBase;
struct DosLibrary *DOSBase;

/* forward declarations */
static void handle_packet(handler_global_t *g, struct DosPacket *pkt);
static void return_packet(handler_global_t *g, struct DosPacket *pkt);
static void mount_volume(handler_global_t *g);
static void unmount_volume(handler_global_t *g);

/* ------------------------------------------------------------------ */
/* Amiga media adapter                                                 */
/* ------------------------------------------------------------------ */
/*
 * TODO: DMA-safe buffers
 *   CDVDFS defaults to MEMF_CHIP for I/O buffers because some SCSI
 *   controllers require DMA-accessible memory. We currently use
 *   whatever AllocMem provides. For broad hardware compatibility,
 *   the read buffer should be allocated with MEMF_CHIP (or at least
 *   MEMF_24BIT on systems with Zorro III). This is deferred until
 *   we can test on real hardware with DMA-sensitive controllers.
 *
 *
 * TODO: DosEnvec control string parsing
 *   CDVDFS parses mount options (LOWERCASE, ROCKRIDGE, JOLIET,
 *   HFSFIRST, SCANINTERVAL, etc.) from the de_Control field in
 *   the DosEnvec. Not needed for ROM-based operation where the
 *   storage driver loads the handler directly. Add if mountlist-
 *   based configuration becomes necessary.
 *
 * TODO: AppIcon / Workbench integration
 *   CDVDFS displays a disc icon on the Workbench desktop when a
 *   CD is inserted and can trigger audio playback on double-click.
 *   This is a cosmetic/convenience feature, not required for
 *   filesystem operation.
 *
 * TODO: AROS integration
 *   AROS may need different endianness handling for HFS structures,
 *   and DirectSCSI may not be available. CDVDFS notes HFS is
 *   "probably broken" on AROS/x86 due to Motorola alignment
 *   assumptions. Test and adapt when AROS target is available.
 */

typedef struct amiga_media_ctx {
    handler_global_t *g;
} amiga_media_ctx_t;

static odfs_err_t amiga_read_sectors(void *ctx, uint32_t lba,
                                      uint32_t count, void *buf)
{
    amiga_media_ctx_t *am = ctx;
    handler_global_t *g = am->g;

    /*
     * Read multiple sectors in one I/O request.
     *
     * For offsets > 4GB (DVD), use TD_READ64 which splits the
     * 64-bit byte offset across io_Offset (low 32) and io_Actual
     * (high 32). For offsets <= 4GB, use CMD_READ.
     *
     * CDVDFS reference: Read_From_Drive() in cdrom.c
     */
    {
        ULONG byte_offset_lo = lba * g->sector_size;
        ULONG byte_offset_hi = 0;

        /* compute 64-bit byte offset: lba * sector_size
         * For 2048-byte sectors: offset = lba << 11
         * High bits: lba >> 21 */
        if (g->sector_size == 2048) {
            byte_offset_lo = lba << 11;
            byte_offset_hi = lba >> 21;
        }

        g->devreq->io_Offset = byte_offset_lo;
        g->devreq->io_Actual = byte_offset_hi;
        g->devreq->io_Length = count * g->sector_size;
        g->devreq->io_Data   = buf;

        if (byte_offset_hi != 0)
            g->devreq->io_Command = TD_READ64;
        else
            g->devreq->io_Command = CMD_READ;
    }

    if (DoIO((struct IORequest *)g->devreq) != 0)
        return ODFS_ERR_IO;

    return ODFS_OK;
}

static uint32_t amiga_sector_size(void *ctx)
{
    amiga_media_ctx_t *am = ctx;
    return am->g->sector_size;
}

static uint32_t amiga_sector_count(void *ctx)
{
    (void)ctx;
    return 0; /* unknown — CD media doesn't reliably report size */
}

static void amiga_close(void *ctx)
{
    (void)ctx; /* device closed in handler shutdown */
}

/*
 * Read TOC via SCSI Read TOC command (0x43).
 * Format 0x02 = full TOC / session info.
 * Falls back to format 0x01 (multisession info) if available.
 */
static odfs_err_t amiga_read_toc(void *ctx, odfs_toc_t *toc)
{
    amiga_media_ctx_t *am = ctx;
    handler_global_t *g = am->g;
    uint8_t cmd[10];
    uint8_t buf[256];
    struct SCSICmd scsi;

    memset(toc, 0, sizeof(*toc));
    memset(cmd, 0, sizeof(cmd));
    memset(buf, 0, sizeof(buf));
    memset(&scsi, 0, sizeof(scsi));

    /* SCSI Read TOC, format 0x00 (TOC) */
    cmd[0] = 0x43;              /* READ TOC/PMA/ATIP */
    cmd[1] = 0x00;              /* MSF=0 (LBA format) */
    cmd[2] = 0x00;              /* format: TOC */
    cmd[6] = 0x01;              /* starting track */
    cmd[7] = (sizeof(buf) >> 8) & 0xFF;
    cmd[8] = sizeof(buf) & 0xFF;

    scsi.scsi_Data = (UWORD *)buf;
    scsi.scsi_Length = sizeof(buf);
    scsi.scsi_CmdLength = 10;
    scsi.scsi_Command = cmd;
    scsi.scsi_Flags = SCSIF_READ | SCSIF_AUTOSENSE;
    scsi.scsi_SenseData = NULL;
    scsi.scsi_SenseLength = 0;

    g->devreq->io_Command = HD_SCSICMD;
    g->devreq->io_Data    = &scsi;
    g->devreq->io_Length  = sizeof(scsi);

    if (DoIO((struct IORequest *)g->devreq) != 0)
        return ODFS_ERR_UNSUPPORTED;

    /* parse TOC response */
    uint16_t toc_len = ((uint16_t)buf[0] << 8) | buf[1];
    uint8_t first_track = buf[2];
    uint8_t last_track = buf[3];
    (void)first_track;

    if (toc_len < 2)
        return ODFS_ERR_BAD_FORMAT;

    /* each TOC descriptor is 8 bytes starting at offset 4 */
    int ndesc = (toc_len - 2) / 8;
    uint8_t session_count = 0;

    for (int i = 0; i < ndesc && i < 99; i++) {
        const uint8_t *desc = &buf[4 + i * 8];
        uint8_t session = desc[0];
        uint8_t adr_ctrl = desc[1];
        uint8_t track = desc[2];
        uint32_t lba = ((uint32_t)desc[4] << 24) |
                       ((uint32_t)desc[5] << 16) |
                       ((uint32_t)desc[6] << 8)  |
                        (uint32_t)desc[7];

        (void)adr_ctrl;

        if (track == 0xAA)
            continue; /* lead-out, skip */

        /* data track check: ctrl & 0x04 */
        if (session_count < 99) {
            toc->sessions[session_count].number = session;
            toc->sessions[session_count].start_lba = lba;
            toc->sessions[session_count].length = 0;
            session_count++;
        }
    }

    toc->session_count = session_count;
    toc->first_session = 1;
    toc->last_session = last_track;

    return (session_count > 0) ? ODFS_OK : ODFS_ERR_BAD_FORMAT;
}

/* ------------------------------------------------------------------ */
/* SCSI helper commands                                                */
/* ------------------------------------------------------------------ */

/*
 * Issue SCSI Test Unit Ready (0x00).
 * Returns 1 if drive is ready, 0 otherwise.
 */
static int scsi_test_unit_ready(handler_global_t *g)
{
    uint8_t cmd[6];
    struct SCSICmd scsi;

    memset(cmd, 0, sizeof(cmd));
    memset(&scsi, 0, sizeof(scsi));

    cmd[0] = 0x00; /* TEST UNIT READY */

    scsi.scsi_Data      = NULL;
    scsi.scsi_Length     = 0;
    scsi.scsi_CmdLength  = 6;
    scsi.scsi_Command    = cmd;
    scsi.scsi_Flags      = SCSIF_AUTOSENSE;

    g->devreq->io_Command = HD_SCSICMD;
    g->devreq->io_Data    = &scsi;
    g->devreq->io_Length  = sizeof(scsi);

    return (DoIO((struct IORequest *)g->devreq) == 0) ? 1 : 0;
}

/*
 * Issue SCSI Mode Select (0x15) to set the block size.
 *
 * This ensures the drive uses 2048-byte blocks (standard for CD-ROM
 * data). Some drives/controllers may default to 512-byte blocks or
 * be left in an odd state after previous operations.
 *
 * CDVDFS reference: Mode_Select() in cdrom.c
 *
 * p_block_length: typically 2048 for CD-ROM data, 2352 for raw audio.
 * Returns 1 on success, 0 on failure (non-fatal).
 */
static int scsi_mode_select(handler_global_t *g, uint32_t block_length)
{
    uint8_t cmd[6];
    uint8_t mode_data[12];
    struct SCSICmd scsi;

    memset(cmd, 0, sizeof(cmd));
    memset(mode_data, 0, sizeof(mode_data));
    memset(&scsi, 0, sizeof(scsi));

    /* MODE SELECT(6) CDB */
    cmd[0] = 0x15;  /* MODE SELECT */
    cmd[1] = 0x10;  /* PF (Page Format) bit set */
    cmd[4] = 12;    /* parameter list length */

    /* Mode parameter header + block descriptor */
    mode_data[3] = 8;  /* block descriptor length */
    /* mode_data[4] = 0; density code (default) */
    /* block length in bytes 9-11 (big-endian) */
    mode_data[9]  = (uint8_t)(block_length >> 16);
    mode_data[10] = (uint8_t)(block_length >> 8);
    mode_data[11] = (uint8_t)(block_length);

    scsi.scsi_Data      = (UWORD *)mode_data;
    scsi.scsi_Length     = sizeof(mode_data);
    scsi.scsi_CmdLength  = 6;
    scsi.scsi_Command    = cmd;
    scsi.scsi_Flags      = SCSIF_WRITE | SCSIF_AUTOSENSE;

    g->devreq->io_Command = HD_SCSICMD;
    g->devreq->io_Data    = &scsi;
    g->devreq->io_Length  = sizeof(scsi);

    return (DoIO((struct IORequest *)g->devreq) == 0) ? 1 : 0;
}

static const odfs_media_ops_t amiga_media_ops = {
    .read_sectors = amiga_read_sectors,
    .sector_size  = amiga_sector_size,
    .sector_count = amiga_sector_count,
    .read_toc     = amiga_read_toc,
    .close        = amiga_close,
};

/* ------------------------------------------------------------------ */
/* log sink                                                            */
/* ------------------------------------------------------------------ */

#if ODFS_SERIAL_DEBUG
static inline void raw_putchar(char c)
{
    register char _d0 __asm("d0") = c;
    register struct ExecBase *_a6 __asm("a6") = SysBase;
    __asm volatile (
        "jsr -516(%%a6)"
        : "+r" (_d0)
        : "r" (_a6)
        : "d1", "a0", "a1", "memory"
    );
}

static void serial_puts(const char *s)
{
    while (*s)
        raw_putchar(*s++);
}

static void log_sink(odfs_log_level_t level, odfs_log_subsys_t subsys,
                     const char *msg, void *ctx)
{
    (void)level;
    (void)subsys;
    (void)ctx;
    serial_puts(msg);
    raw_putchar('\n');
}
#else
static void log_sink(odfs_log_level_t level, odfs_log_subsys_t subsys,
                     const char *msg, void *ctx)
{
    (void)level;
    (void)subsys;
    (void)msg;
    (void)ctx;
}
#endif

/* ------------------------------------------------------------------ */
/* lock management                                                     */
/* ------------------------------------------------------------------ */

static odfs_lock_t *alloc_lock(handler_global_t *g,
                                const odfs_node_t *fnode,
                                const odfs_node_t *parent,
                                LONG access)
{
    odfs_lock_t *ol;

    ol = AllocMem(sizeof(*ol), MEMF_PUBLIC | MEMF_CLEAR);
    if (!ol)
        return NULL;

    ol->fnode = *fnode;
    if (parent)
        ol->parent_node = *parent;
    else
        ol->parent_node = g->mount.root;
    ol->key = g->next_key++;

    ol->lock.fl_Link   = 0;
    ol->lock.fl_Key    = ol->key;
    ol->lock.fl_Access = access;
    ol->lock.fl_Task   = g->dosport;
    ol->lock.fl_Volume = MKBADDR(g->volnode);

    AddTail((struct List *)&g->locklist, (struct Node *)&ol->node);
    return ol;
}

static void free_lock(handler_global_t *g __attribute__((unused)),
                      odfs_lock_t *ol)
{
    if (!ol)
        return;
    Remove((struct Node *)&ol->node);
    FreeMem(ol, sizeof(*ol));
}

static odfs_lock_t *dup_lock(handler_global_t *g, odfs_lock_t *src)
{
    if (!src)
        return NULL;
    return alloc_lock(g, &src->fnode, &src->parent_node, src->lock.fl_Access);
}

/* ------------------------------------------------------------------ */
/* file handle management                                              */
/* ------------------------------------------------------------------ */

static odfs_fh_t *alloc_fh(const odfs_node_t *fnode)
{
    odfs_fh_t *fh;

    fh = AllocMem(sizeof(*fh), MEMF_PUBLIC | MEMF_CLEAR);
    if (!fh)
        return NULL;

    fh->fnode = *fnode;
    fh->pos = 0;
    return fh;
}

static void free_fh(odfs_fh_t *fh)
{
    if (fh)
        FreeMem(fh, sizeof(*fh));
}

/* ------------------------------------------------------------------ */
/* Amiga path resolution                                               */
/* ------------------------------------------------------------------ */

/*
 * Resolve an AmigaDOS path relative to a starting node.
 *
 * AmigaDOS path rules:
 *   "/"          = go to parent
 *   "foo/bar"    = descend into foo, then bar
 *   "foo/bar/"   = descend into foo, bar, then parent of bar
 *   ""           = current node
 *
 * On success, *result is the resolved node and *parent_out is its
 * parent directory node. Returns ODFS_OK or an error.
 */
static odfs_err_t resolve_amiga_path(handler_global_t *g,
                                      const odfs_node_t *start,
                                      const odfs_node_t *start_parent,
                                      const char *path,
                                      odfs_node_t *result,
                                      odfs_node_t *parent_out)
{
    odfs_node_t cur = *start;
    odfs_node_t parent = *start_parent;
    const char *p = path;
    char comp[256];
    odfs_err_t err;

    /* empty path = current node */
    if (*p == '\0') {
        *result = cur;
        *parent_out = parent;
        return ODFS_OK;
    }

    while (*p) {
        /* "/" at current position = go to parent */
        if (*p == '/') {
            /* move up to parent */
            if (cur.extent.lba != g->mount.root.extent.lba) {
                cur = parent;
                parent = g->mount.root; /* grandparent unknown, use root */
            }
            p++;
            continue;
        }

        /* extract next path component */
        const char *end = p;
        while (*end && *end != '/')
            end++;
        int len = (int)(end - p);
        if (len >= (int)sizeof(comp))
            return ODFS_ERR_NAME_TOO_LONG;

        memcpy(comp, p, len);
        comp[len] = '\0';

        /* look up in current directory */
        if (cur.kind != ODFS_NODE_DIR)
            return ODFS_ERR_NOT_DIR;

        parent = cur;

#if ODFS_FEATURE_CDDA
        /* intercept "CDDA" virtual directory on mixed-mode discs */
        if (g->has_cdda && cur.extent.lba == g->mount.root.extent.lba &&
            strcasecmp(comp, "CDDA") == 0) {
            cur = g->cdda_root;
            p = end;
            continue;
        }
#endif

        err = odfs_lookup(&g->mount, &cur, comp, &cur);
        if (err != ODFS_OK)
            return err;

        p = end;
        /* don't skip the '/' here — let the loop handle it
           (so trailing "/" means "go to parent" on next iteration) */
    }

    *result = cur;
    *parent_out = parent;
    return ODFS_OK;
}

/* ------------------------------------------------------------------ */
/* fill FileInfoBlock from odfs_node_t                               */
/* ------------------------------------------------------------------ */

static void fill_fib(struct FileInfoBlock *fib, const odfs_node_t *fnode)
{
    memset(fib, 0, sizeof(*fib));

    /* filename — BCPL string (length prefix) */
    {
        int len = strlen(fnode->name);
        if (len > 106)
            len = 106;
        fib->fib_FileName[0] = len;
        memcpy(&fib->fib_FileName[1], fnode->name, len);
    }

    fib->fib_DirEntryType = (fnode->kind == ODFS_NODE_DIR) ? ST_USERDIR : ST_FILE;
    fib->fib_EntryType    = fib->fib_DirEntryType;
    fib->fib_Size         = (LONG)fnode->size;
    fib->fib_NumBlocks    = (fnode->size + 511) / 512;
    fib->fib_Protection   = FIBF_READ | FIBF_EXECUTE;

    /* date stamp — Amiga epoch is 1978-01-01 */
    if (fnode->mtime.year >= 1978) {
        LONG days = 0;
        int y;
        for (y = 1978; y < fnode->mtime.year; y++) {
            days += 365;
            if ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)
                days++;
        }
        {
            static const int mdays[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
            int m;
            for (m = 1; m < fnode->mtime.month && m <= 12; m++) {
                days += mdays[m];
                if (m == 2 && ((fnode->mtime.year % 4 == 0 &&
                    fnode->mtime.year % 100 != 0) ||
                    fnode->mtime.year % 400 == 0))
                    days++;
            }
        }
        days += fnode->mtime.day - 1;

        fib->fib_Date.ds_Days   = days;
        fib->fib_Date.ds_Minute = fnode->mtime.hour * 60 + fnode->mtime.minute;
        fib->fib_Date.ds_Tick   = fnode->mtime.second * TICKS_PER_SECOND;
    }

    fib->fib_DiskKey = fnode->extent.lba;
}

/* ------------------------------------------------------------------ */
/* packet handlers                                                     */
/* ------------------------------------------------------------------ */

static void action_locate_object(handler_global_t *g, struct DosPacket *pkt)
{
    odfs_lock_t *parent_lock = LOCK_FROM_BPTR(pkt->dp_Arg1);
    LONG access = pkt->dp_Arg3;
    char path[512];
    odfs_node_t result, parent_node;
    odfs_err_t err;
    const odfs_node_t *start;
    const odfs_node_t *start_parent;

    bstr_to_cstr(pkt->dp_Arg2, path, sizeof(path));

    if (parent_lock) {
        start = &parent_lock->fnode;
        start_parent = &parent_lock->parent_node;
    } else {
        start = &g->mount.root;
        start_parent = &g->mount.root;
    }

    err = resolve_amiga_path(g, start, start_parent, path, &result, &parent_node);
    if (err != ODFS_OK) {
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = odfs_err_to_dos(err);
        return;
    }

    odfs_lock_t *ol = alloc_lock(g, &result, &parent_node, access);
    if (!ol) {
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = ERROR_NO_FREE_STORE;
        return;
    }

    pkt->dp_Res1 = LOCK_TO_BPTR(ol);
}

static void action_free_lock(handler_global_t *g, struct DosPacket *pkt)
{
    odfs_lock_t *ol = LOCK_FROM_BPTR(pkt->dp_Arg1);
    free_lock(g, ol);
    pkt->dp_Res1 = DOSTRUE;
}

static void action_copy_dir(handler_global_t *g, struct DosPacket *pkt)
{
    odfs_lock_t *src = LOCK_FROM_BPTR(pkt->dp_Arg1);
    odfs_lock_t *ol;

    if (!src)
        ol = alloc_lock(g, &g->mount.root, &g->mount.root, SHARED_LOCK);
    else
        ol = dup_lock(g, src);

    if (!ol) {
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = ERROR_NO_FREE_STORE;
        return;
    }
    pkt->dp_Res1 = LOCK_TO_BPTR(ol);
}

static void action_parent(handler_global_t *g, struct DosPacket *pkt)
{
    odfs_lock_t *ol = LOCK_FROM_BPTR(pkt->dp_Arg1);

    if (!ol) {
        /* NULL lock = root — root has no parent */
        pkt->dp_Res1 = 0;
        return;
    }

    /* already at root? */
    if (ol->fnode.extent.lba == g->mount.root.extent.lba) {
        pkt->dp_Res1 = 0;
        return;
    }

    /* return a lock on the parent directory */
    odfs_lock_t *parent = alloc_lock(g, &ol->parent_node,
                                      &g->mount.root, /* grandparent = root as fallback */
                                      SHARED_LOCK);
    if (!parent) {
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = ERROR_NO_FREE_STORE;
        return;
    }
    pkt->dp_Res1 = LOCK_TO_BPTR(parent);
}

static void action_same_lock(handler_global_t *g __attribute__((unused)),
                             struct DosPacket *pkt)
{
    odfs_lock_t *l1 = LOCK_FROM_BPTR(pkt->dp_Arg1);
    odfs_lock_t *l2 = LOCK_FROM_BPTR(pkt->dp_Arg2);

    if (l1 && l2 && l1->fnode.extent.lba == l2->fnode.extent.lba)
        pkt->dp_Res1 = DOSTRUE;
    else
        pkt->dp_Res1 = DOSFALSE;
}

/* ---- examine ---- */

/*
 * ExNext uses fib_DiskKey as a byte offset into the directory.
 * readdir resumes from that offset and delivers one entry via
 * the callback, then updates the offset to point past it. O(1)
 * per call instead of O(n).
 */

typedef struct exnext_ctx {
    struct FileInfoBlock *fib;
    int   found;
} exnext_ctx_t;

static odfs_err_t exnext_cb(const odfs_node_t *entry, void *ctx)
{
    exnext_ctx_t *ec = ctx;
    fill_fib(ec->fib, entry);
    ec->found = 1;
    return ODFS_ERR_EOF; /* stop after one entry */
}

static void action_examine_object(handler_global_t *g, struct DosPacket *pkt)
{
    odfs_lock_t *ol = LOCK_FROM_BPTR(pkt->dp_Arg1);
    struct FileInfoBlock *fib = (struct FileInfoBlock *)BADDR(pkt->dp_Arg2);

    fill_fib(fib, ol ? &ol->fnode : &g->mount.root);
    fib->fib_DiskKey = 0; /* reset ExNext resume offset */
    pkt->dp_Res1 = DOSTRUE;
}

static void action_examine_next(handler_global_t *g, struct DosPacket *pkt)
{
    odfs_lock_t *ol = LOCK_FROM_BPTR(pkt->dp_Arg1);
    struct FileInfoBlock *fib = (struct FileInfoBlock *)BADDR(pkt->dp_Arg2);
    const odfs_node_t *dir = ol ? &ol->fnode : &g->mount.root;

    if (dir->kind != ODFS_NODE_DIR) {
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = ERROR_OBJECT_WRONG_TYPE;
        return;
    }

    uint32_t resume = (uint32_t)fib->fib_DiskKey;
    exnext_ctx_t ec;
    ec.fib = fib;
    ec.found = 0;

    /* check if CDDA virtual dir was already emitted (sentinel) */
#if ODFS_FEATURE_CDDA
#define CDDA_EXNEXT_SENTINEL 0x7FFFFFFE
    if (resume == CDDA_EXNEXT_SENTINEL + 1) {
        /* CDDA entry was the last thing we returned — done */
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = ERROR_NO_MORE_ENTRIES;
        return;
    }
#endif

    /* handle CDDA-rooted dir: delegate to CDDA backend */
#if ODFS_FEATURE_CDDA
    if (g->has_cdda && dir->backend == ODFS_BACKEND_CDDA) {
        (void)cdda_backend_ops.readdir(g->cdda_ctx, &g->mount.cache,
                                       &g->log, dir, exnext_cb, &ec, &resume);
        if (ec.found) {
            fib->fib_DiskKey = (LONG)resume;
            pkt->dp_Res1 = DOSTRUE;
        } else {
            pkt->dp_Res1 = DOSFALSE;
            pkt->dp_Res2 = ERROR_NO_MORE_ENTRIES;
        }
        return;
    }
#endif

    (void)odfs_readdir(&g->mount, dir, exnext_cb, &ec, &resume);
    if (ec.found) {
        fib->fib_DiskKey = (LONG)resume;
        pkt->dp_Res1 = DOSTRUE;
    } else {
#if ODFS_FEATURE_CDDA
        /* data entries exhausted — inject CDDA virtual dir if at root */
        if (g->has_cdda && dir->extent.lba == g->mount.root.extent.lba) {
            fill_fib(fib, &g->cdda_root);
            fib->fib_DiskKey = CDDA_EXNEXT_SENTINEL + 1;
            pkt->dp_Res1 = DOSTRUE;
            return;
        }
#endif
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = ERROR_NO_MORE_ENTRIES;
    }
}

static void action_examine_fh(handler_global_t *g __attribute__((unused)),
                               struct DosPacket *pkt)
{
    odfs_fh_t *fh = (odfs_fh_t *)pkt->dp_Arg1;
    struct FileInfoBlock *fib = (struct FileInfoBlock *)BADDR(pkt->dp_Arg2);

    if (!fh) {
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
        return;
    }

    fill_fib(fib, &fh->fnode);
    pkt->dp_Res1 = DOSTRUE;
}

/* ---- file I/O ---- */

static void action_findinput(handler_global_t *g, struct DosPacket *pkt)
{
    struct FileHandle *fhandle = (struct FileHandle *)BADDR(pkt->dp_Arg1);
    odfs_lock_t *dirlock = LOCK_FROM_BPTR(pkt->dp_Arg2);
    char path[512];
    odfs_node_t result, parent_node;
    odfs_err_t err;
    const odfs_node_t *start;
    const odfs_node_t *start_parent;

    bstr_to_cstr(pkt->dp_Arg3, path, sizeof(path));

    if (dirlock) {
        start = &dirlock->fnode;
        start_parent = &dirlock->parent_node;
    } else {
        start = &g->mount.root;
        start_parent = &g->mount.root;
    }

    err = resolve_amiga_path(g, start, start_parent, path, &result, &parent_node);
    if (err != ODFS_OK) {
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = odfs_err_to_dos(err);
        return;
    }

    if (result.kind == ODFS_NODE_DIR) {
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = ERROR_OBJECT_WRONG_TYPE;
        return;
    }

    odfs_fh_t *fh = alloc_fh(&result);
    if (!fh) {
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = ERROR_NO_FREE_STORE;
        return;
    }

    fhandle->fh_Arg1 = (LONG)fh;
    pkt->dp_Res1 = DOSTRUE;
}

static void action_read(handler_global_t *g, struct DosPacket *pkt)
{
    odfs_fh_t *fh = (odfs_fh_t *)pkt->dp_Arg1;
    void *buf = (void *)pkt->dp_Arg2;
    LONG len = pkt->dp_Arg3;

    if (!fh) {
        pkt->dp_Res1 = -1;
        pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
        return;
    }

    size_t actual = (size_t)len;
    odfs_err_t err = odfs_read(&g->mount, &fh->fnode, fh->pos, buf, &actual);
    if (err != ODFS_OK && actual == 0) {
        pkt->dp_Res1 = -1;
        pkt->dp_Res2 = odfs_err_to_dos(err);
        return;
    }

    fh->pos += actual;
    pkt->dp_Res1 = (LONG)actual;
}

static void action_seek(handler_global_t *g __attribute__((unused)),
                        struct DosPacket *pkt)
{
    odfs_fh_t *fh = (odfs_fh_t *)pkt->dp_Arg1;
    LONG offset = pkt->dp_Arg2;
    LONG mode = pkt->dp_Arg3;
    LONG oldpos, newpos;

    if (!fh) {
        pkt->dp_Res1 = -1;
        pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
        return;
    }

    oldpos = (LONG)fh->pos;

    switch (mode) {
    case OFFSET_BEGINNING: newpos = offset; break;
    case OFFSET_CURRENT:   newpos = oldpos + offset; break;
    case OFFSET_END:       newpos = (LONG)fh->fnode.size + offset; break;
    default:
        pkt->dp_Res1 = -1;
        pkt->dp_Res2 = ERROR_SEEK_ERROR;
        return;
    }

    if (newpos < 0 || (ULONG)newpos > fh->fnode.size) {
        pkt->dp_Res1 = -1;
        pkt->dp_Res2 = ERROR_SEEK_ERROR;
        return;
    }

    fh->pos = (ULONG)newpos;
    pkt->dp_Res1 = oldpos;
}

static void action_end(handler_global_t *g __attribute__((unused)),
                       struct DosPacket *pkt)
{
    odfs_fh_t *fh = (odfs_fh_t *)pkt->dp_Arg1;
    free_fh(fh);
    pkt->dp_Res1 = DOSTRUE;
}

/* ---- info ---- */

static void action_disk_info(handler_global_t *g, struct DosPacket *pkt)
{
    struct InfoData *info = (struct InfoData *)BADDR(pkt->dp_Arg1);

    memset(info, 0, sizeof(*info));
    info->id_NumSoftErrors = 0;
    info->id_UnitNumber    = g->devunit;
    info->id_DiskState     = ID_WRITE_PROTECTED;
    info->id_NumBlocks     = odfs_media_sector_count(&g->media);
    info->id_NumBlocksUsed = info->id_NumBlocks;
    info->id_BytesPerBlock = g->sector_size;
    info->id_DiskType      = g->mounted ? ID_DOS_DISK : ID_NO_DISK_PRESENT;
    info->id_VolumeNode    = MKBADDR(g->volnode);
    info->id_InUse         = DOSFALSE;

    pkt->dp_Res1 = DOSTRUE;
}

static void action_is_filesystem(handler_global_t *g __attribute__((unused)),
                                 struct DosPacket *pkt)
{
    pkt->dp_Res1 = DOSTRUE;
}

static void action_current_volume(handler_global_t *g,
                                  struct DosPacket *pkt)
{
    pkt->dp_Res1 = MKBADDR(g->volnode);
}

static void action_inhibit(handler_global_t *g, struct DosPacket *pkt)
{
    LONG state = pkt->dp_Arg1;

    if (state != DOSFALSE) {
        /* inhibit on — unmount, stop I/O */
        g->inhibited = 1;
        unmount_volume(g);
    } else {
        /* inhibit off — try to remount */
        g->inhibited = 0;
        mount_volume(g);
    }
    pkt->dp_Res1 = DOSTRUE;
}

/* ---- write operations (all rejected) ---- */

static void action_write_protected(handler_global_t *g __attribute__((unused)),
                                   struct DosPacket *pkt)
{
    pkt->dp_Res1 = DOSFALSE;
    pkt->dp_Res2 = ERROR_DISK_WRITE_PROTECTED;
}

/* ------------------------------------------------------------------ */
/* packet dispatch                                                     */
/* ------------------------------------------------------------------ */

static void handle_packet(handler_global_t *g, struct DosPacket *pkt)
{
    pkt->dp_Res1 = DOSFALSE;
    pkt->dp_Res2 = 0;

    switch (pkt->dp_Type) {

    /* ---- locks ---- */
    case ACTION_LOCATE_OBJECT:  action_locate_object(g, pkt); break;
    case ACTION_FREE_LOCK:      action_free_lock(g, pkt); break;
    case ACTION_COPY_DIR:       action_copy_dir(g, pkt); break;
    case ACTION_PARENT:         action_parent(g, pkt); break;
    case ACTION_SAME_LOCK:      action_same_lock(g, pkt); break;

    /* ---- examine ---- */
    case ACTION_EXAMINE_OBJECT: action_examine_object(g, pkt); break;
    case ACTION_EXAMINE_NEXT:   action_examine_next(g, pkt); break;
    case ACTION_EXAMINE_FH:     action_examine_fh(g, pkt); break;

    /* ---- file I/O ---- */
    case ACTION_FINDINPUT:      action_findinput(g, pkt); break;
    case ACTION_READ:           action_read(g, pkt); break;
    case ACTION_SEEK:           action_seek(g, pkt); break;
    case ACTION_END:            action_end(g, pkt); break;

    /* ---- info ---- */
    case ACTION_DISK_INFO:      action_disk_info(g, pkt); break;
    case ACTION_INFO:           action_disk_info(g, pkt); break;
    case ACTION_IS_FILESYSTEM:  action_is_filesystem(g, pkt); break;
    case ACTION_CURRENT_VOLUME: action_current_volume(g, pkt); break;
    case ACTION_INHIBIT:        action_inhibit(g, pkt); break;

    /* ---- FH variants ---- */
    case ACTION_COPY_DIR_FH:    action_copy_dir(g, pkt); break;
    case ACTION_PARENT_FH:      action_parent(g, pkt); break;
    case ACTION_FH_FROM_LOCK: {
        odfs_lock_t *ol = LOCK_FROM_BPTR(pkt->dp_Arg2);
        if (ol) {
            struct FileHandle *fhandle = (struct FileHandle *)BADDR(pkt->dp_Arg1);
            odfs_fh_t *fh = alloc_fh(&ol->fnode);
            if (fh) {
                fhandle->fh_Arg1 = (LONG)fh;
                free_lock(g, ol);
                pkt->dp_Res1 = DOSTRUE;
            } else {
                pkt->dp_Res2 = ERROR_NO_FREE_STORE;
            }
        } else {
            pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
        }
        break;
    }

    /* ---- read-only: reject writes ---- */
    case ACTION_FINDOUTPUT:
    case ACTION_FINDUPDATE:
    case ACTION_WRITE:
    case ACTION_DELETE_OBJECT:
    case ACTION_RENAME_OBJECT:
    case ACTION_CREATE_DIR:
    case ACTION_SET_PROTECT:
    case ACTION_SET_COMMENT:
    case ACTION_RENAME_DISK:
    case ACTION_SET_DATE:
    case ACTION_SET_FILE_SIZE:
    case ACTION_SET_OWNER:
        action_write_protected(g, pkt);
        break;

    /* ---- nops ---- */
    case ACTION_FLUSH:
    case ACTION_MORE_CACHE:
        pkt->dp_Res1 = DOSTRUE;
        break;

    /* ---- shutdown ---- */
    case ACTION_DIE:
        pkt->dp_Res1 = DOSTRUE;
        break;

    default:
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = ERROR_ACTION_NOT_KNOWN;
        break;
    }
}

static void return_packet(handler_global_t *g, struct DosPacket *pkt)
{
    struct MsgPort *replyport = pkt->dp_Port;
    struct Message *msg = pkt->dp_Link;

    pkt->dp_Port = g->dosport;
    msg->mn_Node.ln_Name = (char *)pkt;
    msg->mn_Node.ln_Succ = NULL;
    msg->mn_Node.ln_Pred = NULL;
    PutMsg(replyport, msg);
}

/* ------------------------------------------------------------------ */
/* volume mount / unmount                                              */
/* ------------------------------------------------------------------ */

static struct DeviceList *create_volume_node(handler_global_t *g)
{
    struct DeviceList *dl;
    int namelen;
    UBYTE *bname;

    dl = (struct DeviceList *)AllocMem(sizeof(*dl), MEMF_PUBLIC | MEMF_CLEAR);
    if (!dl)
        return NULL;

    namelen = strlen(g->volname);
    if (namelen > 30)
        namelen = 30;
    bname = AllocMem(namelen + 2, MEMF_PUBLIC | MEMF_CLEAR);
    if (!bname) {
        FreeMem(dl, sizeof(*dl));
        return NULL;
    }
    bname[0] = namelen;
    memcpy(bname + 1, g->volname, namelen);
    bname[namelen + 1] = '\0';

    dl->dl_Type     = DLT_VOLUME;
    dl->dl_Task     = g->dosport;
    dl->dl_Lock     = 0;
    dl->dl_DiskType = ID_DOS_DISK;
    dl->dl_Name     = MKBADDR(bname);

    return dl;
}

static void mount_volume(handler_global_t *g)
{
    odfs_mount_opts_t opts;
    odfs_err_t err;

    if (g->mounted || g->inhibited)
        return;

    odfs_mount_opts_default(&opts);
    err = odfs_mount(&g->media, &opts, &g->log, &g->mount);
    if (err != ODFS_OK) {
#if ODFS_FEATURE_CDDA
        /* no data filesystem — try pure audio CD */
        odfs_toc_t toc;
        if (odfs_media_read_toc(&g->media, &toc) == ODFS_OK &&
            cdda_mount_from_toc(&toc, 0, &g->cdda_root,
                                &g->cdda_ctx) == ODFS_OK) {
            g->has_cdda = 1;
            g->mounted = 1;
            g->mount.root = g->cdda_root;
            g->mount.backend_ops = &cdda_backend_ops;
            g->mount.backend_ctx = g->cdda_ctx;
            g->mount.active_backend = ODFS_BACKEND_CDDA;
            memcpy(g->volname, "Audio CD", 9);
        }
#endif
        if (!g->mounted)
            return;
    } else {
        g->mounted = 1;
        memcpy(g->volname, g->mount.volume_name, sizeof(g->volname) - 1);
        g->volname[sizeof(g->volname) - 1] = '\0';
        if (g->volname[0] == '\0')
            memcpy(g->volname, "Unnamed", 8);

#if ODFS_FEATURE_CDDA
        /* check for audio tracks on mixed-mode disc */
        {
            odfs_toc_t toc;
            if (odfs_media_read_toc(&g->media, &toc) == ODFS_OK &&
                cdda_mount_from_toc(&toc, 1, &g->cdda_root,
                                    &g->cdda_ctx) == ODFS_OK) {
                g->has_cdda = 1;
            }
        }
#endif
    }

    g->volnode = create_volume_node(g);
    if (g->volnode) {
        if (AttemptLockDosList(LDF_VOLUMES | LDF_WRITE)) {
            AddDosEntry((struct DosList *)g->volnode);
            UnLockDosList(LDF_VOLUMES | LDF_WRITE);
        }
    }
}

static void unmount_volume(handler_global_t *g)
{
    if (!g->mounted)
        return;

    /* free all locks (they become stale on media change) */
    {
        struct Node *node;
        while ((node = RemHead((struct List *)&g->locklist)) != NULL)
            FreeMem(node, sizeof(odfs_lock_t));
    }

#if ODFS_FEATURE_CDDA
    /* free CDDA context if separate from main mount */
    if (g->has_cdda && g->cdda_ctx &&
        g->cdda_ctx != g->mount.backend_ctx) {
        cdda_backend_ops.unmount(g->cdda_ctx);
    }
    g->cdda_ctx = NULL;
    g->has_cdda = 0;
#endif

    odfs_unmount(&g->mount);
    g->mounted = 0;

    if (g->volnode) {
        if (AttemptLockDosList(LDF_VOLUMES | LDF_WRITE)) {
            RemDosEntry((struct DosList *)g->volnode);
            UnLockDosList(LDF_VOLUMES | LDF_WRITE);
        }
        {
            UBYTE *bname = (UBYTE *)BADDR(g->volnode->dl_Name);
            if (bname)
                FreeMem(bname, bname[0] + 2);
        }
        FreeMem(g->volnode, sizeof(struct DeviceList));
        g->volnode = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* media change detection                                              */
/* ------------------------------------------------------------------ */

static void install_media_change(handler_global_t *g)
{
    g->chgsigbit = AllocSignal(-1);
    if (g->chgsigbit == -1)
        return;

    g->chgport = CreateMsgPort();
    if (!g->chgport) {
        FreeSignal(g->chgsigbit);
        g->chgsigbit = -1;
        return;
    }

    g->chgreq = (struct IOStdReq *)CreateIORequest(g->chgport,
                                                    sizeof(struct IOStdReq));
    if (!g->chgreq) {
        DeleteMsgPort(g->chgport);
        g->chgport = NULL;
        FreeSignal(g->chgsigbit);
        g->chgsigbit = -1;
        return;
    }

    /* clone the device from the main request */
    g->chgreq->io_Device = g->devreq->io_Device;
    g->chgreq->io_Unit   = g->devreq->io_Unit;

    /* install changeint */
    static struct Interrupt chgint;
    chgint.is_Node.ln_Type = NT_INTERRUPT;
    chgint.is_Node.ln_Pri  = 0;
    chgint.is_Node.ln_Name = (char *)"odfs-mediachange";
    chgint.is_Data          = FindTask(NULL);
    /* Amiga API requires old-style function pointer cast */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-prototypes"
    chgint.is_Code          = (void (*)())&Signal;
#pragma GCC diagnostic pop
    /* is_Code will be called as: is_Code(is_Data, 1 << chgsigbit) */

    g->chgreq->io_Command = TD_ADDCHANGEINT;
    g->chgreq->io_Data    = (APTR)&chgint;
    g->chgreq->io_Length  = sizeof(chgint);
    g->chgreq->io_Flags   = 0;

    SendIO((struct IORequest *)g->chgreq);
    g->chg_installed = 1;
}

static void remove_media_change(handler_global_t *g)
{
    if (!g->chg_installed)
        return;

    g->chgreq->io_Command = TD_REMCHANGEINT;
    DoIO((struct IORequest *)g->chgreq);
    g->chg_installed = 0;

    if (g->chgreq) {
        /* don't CloseDevice — we don't own it */
        g->chgreq->io_Device = NULL;
        g->chgreq->io_Unit = NULL;
        DeleteIORequest((struct IORequest *)g->chgreq);
        g->chgreq = NULL;
    }
    if (g->chgport) {
        DeleteMsgPort(g->chgport);
        g->chgport = NULL;
    }
    if (g->chgsigbit != -1) {
        FreeSignal(g->chgsigbit);
        g->chgsigbit = -1;
    }
}

static void handle_media_change(handler_global_t *g)
{
    ULONG status;

    /* check if disc is present */
    g->devreq->io_Command = TD_CHANGESTATE;
    DoIO((struct IORequest *)g->devreq);
    status = g->devreq->io_Actual;

    if (status != 0) {
        /* no disc present */
        unmount_volume(g);
    } else {
        /* disc present — unmount old, try mount new */
        unmount_volume(g);
        /* re-init media adapter since cache was destroyed */
        mount_volume(g);
    }
}

/* ------------------------------------------------------------------ */
/* handler main entry point                                            */
/* ------------------------------------------------------------------ */

void handler_main(void)
{
    handler_global_t *g;
    struct Message *msg;
    struct DosPacket *pkt;
    struct FileSysStartupMsg *fssm;
    struct DosEnvec *de;
    ULONG dossig, chgsig, waitmask;
    int running = 1;
    static amiga_media_ctx_t amctx;

    (void)version_string; /* ensure $VER is not optimized out */

    SysBase = *((struct ExecBase **)4L);

    g = AllocMem(sizeof(*g), MEMF_PUBLIC | MEMF_CLEAR);
    if (!g)
        return;

    g->sysbase = SysBase;
    g->locklist.mlh_Head     = (struct MinNode *)&g->locklist.mlh_Tail;
    g->locklist.mlh_Tail     = NULL;
    g->locklist.mlh_TailPred = (struct MinNode *)&g->locklist.mlh_Head;
    g->next_key = 1;
    g->chgsigbit = -1;

    {
        struct Process *proc = (struct Process *)FindTask(NULL);
        g->dosport = &proc->pr_MsgPort;
    }

    /* wait for startup packet */
    WaitPort(g->dosport);
    msg = GetMsg(g->dosport);
    pkt = (struct DosPacket *)msg->mn_Node.ln_Name;

    g->devnode = (struct DeviceNode *)BADDR(pkt->dp_Arg3);
    fssm = (struct FileSysStartupMsg *)BADDR(pkt->dp_Arg2);

    DOSBase = (struct DosLibrary *)OpenLibrary((CONST_STRPTR)"dos.library", 36);
    if (!DOSBase) {
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = ERROR_INVALID_RESIDENT_LIBRARY;
        return_packet(g, pkt);
        FreeMem(g, sizeof(*g));
        return;
    }
    g->dosbase = DOSBase;

    /* parse FSSM */
    {
        const UBYTE *bdevname = (const UBYTE *)BADDR(fssm->fssm_Device);
        int len = bdevname[0];
        if (len >= (int)sizeof(g->devname))
            len = sizeof(g->devname) - 1;
        memcpy(g->devname, bdevname + 1, len);
        g->devname[len] = '\0';
    }
    g->devunit = fssm->fssm_Unit;
    g->devflags = fssm->fssm_Flags;

    de = (struct DosEnvec *)BADDR(fssm->fssm_Environ);
    g->sector_size = de->de_SizeBlock << 2;

    /* open device */
    g->devport = CreateMsgPort();
    g->devreq = (struct IOStdReq *)CreateIORequest(g->devport,
                                                    sizeof(struct IOStdReq));
    if (!g->devport || !g->devreq ||
        OpenDevice((CONST_STRPTR)g->devname, g->devunit,
                   (struct IORequest *)g->devreq, g->devflags) != 0) {
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = ERROR_DEVICE_NOT_MOUNTED;
        return_packet(g, pkt);
        goto shutdown;
    }

    g->devnode->dn_Task = g->dosport;

    /*
     * SCSI drive setup: wait for unit ready and set 2048-byte blocks.
     * Mode Select may fail on non-SCSI devices (e.g. IDE with
     * trackdisk.device) — this is non-fatal.
     */
    scsi_test_unit_ready(g);
    if (!scsi_mode_select(g, 2048)) {
        /* Mode Select failed — drive probably doesn't support it
         * or is already in 2048-byte mode. Not fatal. */
    }

    /* set up media adapter */
    amctx.g = g;
    g->media.ops = &amiga_media_ops;
    g->media.ctx = &amctx;

    /* set up logging */
    odfs_log_init(&g->log);
    odfs_log_set_sink(&g->log, log_sink, NULL);
    odfs_log_set_level(&g->log, ODFS_LOG_INFO);

    /* mount filesystem */
    mount_volume(g);

    /* install media change interrupt */
    install_media_change(g);

    /* reply startup packet */
    pkt->dp_Res1 = DOSTRUE;
    pkt->dp_Res2 = 0;
    return_packet(g, pkt);

    /* ---- main packet loop ---- */
    dossig = 1UL << g->dosport->mp_SigBit;
    chgsig = (g->chgsigbit >= 0) ? (1UL << g->chgsigbit) : 0;
    waitmask = dossig | chgsig;

    while (running) {
        ULONG sigs = Wait(waitmask);

        /* media change */
        if ((sigs & chgsig) && !g->inhibited) {
            handle_media_change(g);
            /* re-init media adapter after remount */
            amctx.g = g;
        }

        /* DOS packets */
        if (sigs & dossig) {
            while ((msg = GetMsg(g->dosport)) != NULL) {
                pkt = (struct DosPacket *)msg->mn_Node.ln_Name;

                if (pkt->dp_Type == ACTION_DIE) {
                    pkt->dp_Res1 = DOSTRUE;
                    pkt->dp_Res2 = 0;
                    return_packet(g, pkt);
                    running = 0;
                    break;
                }

                if (!g->mounted &&
                    pkt->dp_Type != ACTION_IS_FILESYSTEM &&
                    pkt->dp_Type != ACTION_INHIBIT &&
                    pkt->dp_Type != ACTION_DISK_INFO &&
                    pkt->dp_Type != ACTION_INFO) {
                    pkt->dp_Res1 = DOSFALSE;
                    pkt->dp_Res2 = ERROR_NO_DISK;
                    return_packet(g, pkt);
                    continue;
                }

                handle_packet(g, pkt);
                return_packet(g, pkt);
            }
        }
    }

    /* ---- shutdown ---- */
    remove_media_change(g);
    unmount_volume(g);

shutdown:
    if (g->devreq) {
        if (g->devreq->io_Device)
            CloseDevice((struct IORequest *)g->devreq);
        DeleteIORequest((struct IORequest *)g->devreq);
    }
    if (g->devport)
        DeleteMsgPort(g->devport);

    if (g->devnode)
        g->devnode->dn_Task = NULL;

    CloseLibrary((struct Library *)DOSBase);
    FreeMem(g, sizeof(*g));
}
