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
#include "sys_compat.h"

#if ODFS_AMIGA_OS4
#include "vector_port.h"
/*
 * OS4 vector callbacks run in the calling process context; the handler
 * process must hold the same semaphore while it touches handler state.
 */
#define ODFS_FS_LOCK(g)   ObtainSemaphore(&(g)->fs_sem)
#define ODFS_FS_UNLOCK(g) ReleaseSemaphore(&(g)->fs_sem)
#else
#define ODFS_FS_LOCK(g)   ((void)0)
#define ODFS_FS_UNLOCK(g) ((void)0)
#endif

#if ODFS_FEATURE_CDDA
#include "cdda/cdda.h"
#endif

#include <exec/execbase.h>
#include <exec/errors.h>
#include <exec/io.h>
#include <devices/trackdisk.h>
#include <devices/scsidisk.h>
#include <devices/input.h>
#include <devices/inputevent.h>
#include <devices/timer.h>
#include <dos/dostags.h>
#include <dos/exall.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include <string.h>

#include "odfs/error.h"
#include "odfs/ancestry.h"
#include "odfs/alloc.h"
#include "odfs/charset.h"
#include "odfs/string.h"

#ifndef ODFS_GIT_VERSION
#define ODFS_GIT_VERSION "unknown"
#endif

/*
 * DismountDevice() flags, carried in ACTION_SHUTDOWN's dp_Arg1 (V51+).
 * The m68k NDK predates them, so define locally when the SDK does not.
 * Only DMDF_KEEPDEVICE changes our teardown: leave the device node
 * published (for a later remount) instead of removing it from the list.
 */
#ifndef DMDF_KEEPDEVICE
#define DMDF_KEEPDEVICE   (1L << 0)
#endif
#ifndef DMDF_REMOVEDEVICE
#define DMDF_REMOVEDEVICE (1L << 1)
#endif

const char version_string[] __attribute__((used)) =
    "$VER: ODFileSystem " ODFS_GIT_VERSION
    " (" ODFS_AMIGA_DATE ")";

#define VOLUME_PUBLISH_RETRY_MICROS 100000UL

#if ODFS_AMIGA_OS4
typedef struct TimeRequest odfs_timer_request_t;
#define ODFS_TIMER_IO(tr)       (&(tr)->Request)
#define ODFS_TIMER_SECONDS(tr)  ((tr)->Time.Seconds)
#define ODFS_TIMER_MICROS(tr)   ((tr)->Time.Microseconds)
#else
typedef struct timerequest odfs_timer_request_t;
#define ODFS_TIMER_IO(tr)       (&(tr)->tr_node)
#define ODFS_TIMER_SECONDS(tr)  ((tr)->tr_time.tv_secs)
#define ODFS_TIMER_MICROS(tr)   ((tr)->tr_time.tv_micro)
#endif

/* forward declarations */
static void handle_packet(handler_global_t *g, struct DosPacket *pkt);
static void return_packet(handler_global_t *g, struct DosPacket *pkt);
static void publish_device_node(handler_global_t *g);
static void unpublish_device_node(handler_global_t *g, int keep_device);
#if ODFS_AMIGA_OS4
static LONG activate_vector_port(handler_global_t *g);
static void deactivate_vector_port(handler_global_t *g);
#endif
static void mount_volume(handler_global_t *g);
static void unmount_volume(handler_global_t *g);
static void free_volume(odfs_volume_t *volume);
static void destroy_device_node(struct DeviceNode *devnode);
static void destroy_volume_node(struct DeviceList *volnode);
static int detach_volume_node(odfs_volume_t *volume);
static int publish_volume_node(handler_global_t *g);
static void schedule_volume_publish_retry(handler_global_t *g);
static void cancel_volume_publish_retry(handler_global_t *g);
static void handle_volume_publish_retry(handler_global_t *g);
static void destroy_volume_publish_timer(handler_global_t *g);
static void reap_stale_volumes(handler_global_t *g);
static int node_is_mount_root(const handler_global_t *g, const odfs_node_t *fnode);
static int query_media_change_count(handler_global_t *g, ULONG *count);
static int query_media_present(handler_global_t *g, ULONG *status);
static void fill_volume_date(handler_global_t *g, struct DateStamp *stamp);
static void notify_workbench_disk_change(BOOL inserted);
#if ODFS_FEATURE_CDDA
static int toc_has_data_track(const odfs_toc_t *toc);
static void copy_pure_audio_volume_name(handler_global_t *g);
static void load_cdda_disk_icon(handler_global_t *g);
static void finish_pure_audio_mount(handler_global_t *g);
#endif
static int scsi_is_unsupported_command(const uint8_t *sense);


/* ------------------------------------------------------------------ */
/* Amiga media adapter                                                 */
/* ------------------------------------------------------------------ */
/*
 * DMA-safe bounce buffer:
 *   All device I/O goes through a pre-allocated, 16-byte aligned
 *   buffer allocated with de_BufMemType from the DosEnvec. This
 *   ensures the buffer is in DMA-accessible memory (MEMF_CHIP on
 *   systems with DMA-sensitive SCSI controllers). Data is copied
 *   from the bounce buffer to the caller's buffer after each read.
 *   The overhead is one memcpy per cache miss — negligible compared
 *   to CD seek times.
 *
 *   CDVDFS and PFS3AIO both use de_BufMemType for I/O buffers.
 *   CDVDFS additionally aligns to 16 bytes for 68040 DMA performance.
 *
 *
 * AROS compatibility:
 *   BSTR/BPTR access uses AROS_BSTR_ADDR/AROS_BSTR_strlen macros
 *   from aros_compat.h, which resolve to AROS or classic AmigaOS
 *   implementations depending on __AROS__. All on-disc structure
 *   parsing uses explicit byte-level access, so endianness is
 *   handled correctly on both big-endian (m68k) and little-endian
 *   (x86 AROS) targets.
 */

/* amiga_media_ctx_t is defined in handler.h and embedded per-process
 * in handler_global so concurrent handler processes never share it. */

static int scsi_is_unsupported_command(const uint8_t *sense)
{
    if (!sense)
        return 0;

    return ((sense[2] & 0x0f) == 0x05 && sense[12] == 0x20);
}

#if !ODFS_AMIGA_OS4
static int amiga_direct_read_window_ok(handler_global_t *g,
                                       const void *buf,
                                       uint32_t len)
{
    ULONG start;
    ULONG end;
    ULONG win_start;
    ULONG win_end;

    if (!g->direct_read_buf || g->direct_read_len == 0 || len == 0)
        return 0;

    start = (ULONG)buf;
    end = start + len - 1;
    win_start = (ULONG)g->direct_read_buf;
    win_end = win_start + g->direct_read_len - 1;
    if (end < start || win_end < win_start)
        return 0;

    return start >= win_start && end <= win_end;
}

static int amiga_direct_memtype_ok(ULONG memtype, const void *buf,
                                   uint32_t len)
{
    ULONG need = memtype & (MEMF_PUBLIC | MEMF_CHIP | MEMF_FAST |
                            MEMF_LOCAL | MEMF_24BITDMA | MEMF_KICK);
    ULONG start_type;
    ULONG end_type;

    if (need == MEMF_ANY)
        return 1;

    start_type = TypeOfMem((CONST_APTR)buf);
    end_type = TypeOfMem((CONST_APTR)((ULONG)buf + len - 1));
    return ((start_type & need) == need) && ((end_type & need) == need);
}

static int amiga_can_read_direct(handler_global_t *g, const void *buf,
                                 uint32_t len, uint32_t sectors)
{
    struct DosEnvec *de;
    ULONG start;
    ULONG end;
    ULONG blocked;

    if (!g || !g->envec || !buf || len == 0 || sectors < 2)
        return 0;
    if (!amiga_direct_read_window_ok(g, buf, len))
        return 0;

    de = g->envec;
    if (de->de_MaxTransfer != 0 && len > de->de_MaxTransfer)
        return 0;

    start = (ULONG)buf;
    end = start + len - 1;
    if (end < start)
        return 0;

    blocked = ~de->de_Mask;
    if ((start & blocked) != 0)
        return 0;

    return amiga_direct_memtype_ok(de->de_BufMemType, buf, len);
}

static odfs_err_t amiga_read_hi(handler_global_t *g,
                                struct IOStdReq *req,
                                uint32_t lba,
                                uint32_t bytes,
                                void *buf);

static odfs_err_t amiga_read_direct(handler_global_t *g,
                                    struct IOStdReq *req,
                                    uint32_t lba,
                                    uint32_t count,
                                    uint32_t bytes,
                                    void *buf)
{
    ULONG byte_offset_lo;
    ULONG byte_offset_hi = 0;

    (void)count; /* used by diagnostics when logging is enabled */

    if (g->sector_size == 2048) {
        byte_offset_lo = lba << 11;
        byte_offset_hi = lba >> 21;
    } else {
        byte_offset_lo = lba * g->sector_size;
    }

    if (byte_offset_hi != 0)
        return amiga_read_hi(g, req, lba, bytes, buf);

    req->io_Offset = byte_offset_lo;
    req->io_Actual = 0;
    req->io_Length = bytes;
    req->io_Data = buf;
    req->io_Command = CMD_READ;

    if (DoIO((struct IORequest *)req) != 0 ||
        req->io_Error != 0 ||
        req->io_Actual != bytes) {
        ODFS_ERROR(&g->log, ODFS_SUB_IO,
                   "direct read failed unit=%lu lba=%lu count=%lu "
                   "bytes=%lu off=%lu io_Error=%ld actual=%lu cmd=%lu",
                   (unsigned long)g->devunit,
                   (unsigned long)lba,
                   (unsigned long)count,
                   (unsigned long)bytes,
                   (unsigned long)req->io_Offset,
                   (long)req->io_Error,
                   (unsigned long)req->io_Actual,
                   (unsigned long)req->io_Command);
        return ODFS_ERR_IO;
    }

    return ODFS_OK;
}
#endif

#ifndef NSCMD_TD_READ64
#define NSCMD_TD_READ64 0xC000  /* devices/newstyle.h */
#endif

static void scsi_init_read(struct SCSICmd *scsi,
                           void *data, ULONG data_len,
                           UBYTE *command, UWORD command_len,
                           UBYTE *sense, UWORD sense_len)
{
    memset(scsi, 0, sizeof(*scsi));
    scsi->scsi_Data = (UWORD *)data;
    scsi->scsi_Length = data_len;
    scsi->scsi_CmdLength = command_len;
    scsi->scsi_Command = command;
    scsi->scsi_Flags = SCSIF_READ | SCSIF_AUTOSENSE;
    scsi->scsi_SenseData = sense;
    scsi->scsi_SenseLength = sense_len;
}

/*
 * Issue a SCSI READ(10) through HD_SCSICMD. The workhorse fallback for
 * byte offsets past 4 GiB on drivers that implement neither TD64 nor
 * NSD 64-bit commands: READ(10) addresses by block number, so the
 * 32-bit byte-offset limit of CMD_READ never enters the picture. Every
 * ATAPI/SCSI CD/DVD drive speaks it.
 */
static odfs_err_t scsi_read10(handler_global_t *g,
                              struct IOStdReq *req,
                              uint32_t lba,
                              uint32_t bytes,
                              void *buf,
                              LONG *io_error_out)
{
    uint8_t cmd[10];
    uint8_t sense[32];
    struct SCSICmd scsi;
    uint32_t blocks = bytes / 2048;
    LONG io_rc;

    (void)g;

    memset(cmd, 0, sizeof(cmd));
    memset(sense, 0, sizeof(sense));

    cmd[0] = 0x28; /* READ(10) */
    cmd[2] = (uint8_t)(lba >> 24);
    cmd[3] = (uint8_t)(lba >> 16);
    cmd[4] = (uint8_t)(lba >> 8);
    cmd[5] = (uint8_t)lba;
    cmd[7] = (uint8_t)(blocks >> 8);
    cmd[8] = (uint8_t)blocks;

    scsi_init_read(&scsi, buf, bytes, cmd, sizeof(cmd),
                   sense, sizeof(sense));

    req->io_Command = HD_SCSICMD;
    req->io_Data    = &scsi;
    req->io_Length  = sizeof(scsi);

    io_rc = DoIO((struct IORequest *)req);
    if (io_error_out)
        *io_error_out = req->io_Error;
    if (io_rc != 0 || req->io_Error != 0 || scsi.scsi_Status != 0 ||
        scsi.scsi_Actual != bytes)
        return ODFS_ERR_IO;

    return ODFS_OK;
}

/*
 * Read bytes at a byte offset of 4 GiB or more (only reachable with
 * 2048-byte sectors, so lba > 0x1FFFFF). CMD_READ's ULONG io_Offset
 * cannot express the offset; it silently wraps — reading the wrong
 * sectors — so a 64-bit-capable path is mandatory here.
 *
 * Drivers disagree on which one they provide (issue #7:
 * telmexatapi.device answers TD_READ64 with IOERR_NOCMD). Probe
 * TD_READ64, then NSCMD_TD_READ64, then fall back to SCSI READ(10),
 * and remember the first method that works for the rest of the mount.
 * Only probe rejections (IOERR_NOCMD) advance the chain: once a method
 * has been selected, a failure is a real medium error and is reported,
 * not papered over by switching commands.
 */
static odfs_err_t amiga_read_hi(handler_global_t *g,
                                struct IOStdReq *req,
                                uint32_t lba,
                                uint32_t bytes,
                                void *buf)
{
    int mode = g->read64_mode;
    int probing = (mode == ODFS_READ64_UNPROBED);
    LONG io_error = 0;

    if (mode == ODFS_READ64_NONE)
        return ODFS_ERR_UNSUPPORTED;
    if (probing)
        mode = ODFS_READ64_TD64;

    for (;;) {
        odfs_err_t err;

        if (mode == ODFS_READ64_TD64 || mode == ODFS_READ64_NSD) {
            req->io_Command = (mode == ODFS_READ64_TD64) ? TD_READ64
                                                         : NSCMD_TD_READ64;
            req->io_Offset  = lba << 11;
            req->io_Actual  = lba >> 21; /* high 32 bits of byte offset */
            req->io_Length  = bytes;
            req->io_Data    = buf;

            if (DoIO((struct IORequest *)req) == 0 &&
                req->io_Error == 0 &&
                req->io_Actual == bytes)
                err = ODFS_OK;
            else {
                io_error = req->io_Error;
                err = ODFS_ERR_IO;
            }
        } else {
            err = scsi_read10(g, req, lba, bytes, buf, &io_error);
        }

        if (err == ODFS_OK) {
            if (probing) {
                g->read64_mode = mode;
                ODFS_INFO(&g->log, ODFS_SUB_IO,
                          "reads past 4 GiB use %s",
                          mode == ODFS_READ64_TD64 ? "TD_READ64" :
                          mode == ODFS_READ64_NSD  ? "NSCMD_TD_READ64" :
                                                     "SCSI READ(10)");
            }
            return ODFS_OK;
        }

        /* an unsupported command only advances the probe chain */
        if (probing && io_error == IOERR_NOCMD &&
            mode != ODFS_READ64_SCSI) {
            mode++;
            continue;
        }

        if (probing && mode == ODFS_READ64_SCSI &&
            io_error == IOERR_NOCMD) {
            g->read64_mode = ODFS_READ64_NONE;
            ODFS_ERROR(&g->log, ODFS_SUB_IO,
                       "device offers no way to read past 4 GiB "
                       "(TD64/NSD/HD_SCSICMD all rejected); large "
                       "media will be partly unreadable");
            return ODFS_ERR_UNSUPPORTED;
        }

        ODFS_ERROR(&g->log, ODFS_SUB_IO,
                   "64-bit read failed unit=%lu lba=%lu bytes=%lu "
                   "mode=%d io_Error=%ld",
                   (unsigned long)g->devunit,
                   (unsigned long)lba,
                   (unsigned long)bytes,
                   mode,
                   (long)io_error);
        return ODFS_ERR_IO;
    }
}

static LONG changeint_signal(APTR data)
{
    odfs_changeint_data_t *ci = data;

    if (ci && ci->task && ci->sigmask)
        Signal(ci->task, ci->sigmask);
    return 0;
}

/*
 * Days from 1978-01-01 (the AmigaDOS epoch) for a civil date, computed
 * in closed form. A per-year loop here costs ~150 divisions per
 * conversion, and a conversion runs for every FileInfoBlock filled by
 * Examine, ExNext, and ExAll.
 */
static LONG days_since_1978(int year, int month, int day)
{
    LONG y = year;
    LONG era, yoe, doy, doe;

    /* shift so the year starts in March; Jan/Feb belong to y-1 */
    y -= month <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;                          /* [0, 399] */
    doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;  /* [0, 146096] */

    /* 719468 days from 0000-03-01 to 1970-01-01, 2922 more to 1978 */
    return era * 146097 + doe - 719468 - 2922;
}

static int odfs_timestamp_to_datestamp(const odfs_timestamp_t *ts,
                                       struct DateStamp *stamp)
{
    if (!ts || !stamp || ts->year < 1978 || ts->month < 1 || ts->month > 12 ||
        ts->day < 1 || ts->day > 31 || ts->hour > 23 || ts->minute > 59 ||
        ts->second > 59)
        return 0;

    stamp->ds_Days = days_since_1978(ts->year, ts->month, ts->day);
    stamp->ds_Minute = ts->hour * 60 + ts->minute;
    stamp->ds_Tick = ts->second * TICKS_PER_SECOND;
    return 1;
}

static void fill_volume_date(handler_global_t *g, struct DateStamp *stamp)
{
    if (!stamp)
        return;

    memset(stamp, 0, sizeof(*stamp));

    /* Workbench keys mounted volumes by name plus dl_VolumeDate. Prefer
     * on-disc timestamps when the active backend provides them. */
    if (odfs_timestamp_to_datestamp(&g->mount.root.ctime, stamp))
        return;
    if (odfs_timestamp_to_datestamp(&g->mount.root.mtime, stamp))
        return;

    /* Synthetic backends like pure-audio CDDA have no on-disc root date.
     * Use the current time so each inserted medium still gets a fresh
     * identity instead of reusing a stale zero stamp. */
    DateStamp(stamp);
}

static void notify_workbench_disk_change(BOOL inserted)
{
    struct MsgPort *port;
    struct IOStdReq *req;
    struct InputEvent event;

    port = odfs_amiga_create_msg_port();
    if (!port)
        return;

    req = (struct IOStdReq *)odfs_amiga_create_io_request(port, sizeof(*req));
    if (!req) {
        odfs_amiga_delete_msg_port(port);
        return;
    }

    if (OpenDevice((CONST_STRPTR)"input.device", 0,
                   (struct IORequest *)req, 0) == 0) {
        memset(&event, 0, sizeof(event));
        event.ie_Class = inserted ? IECLASS_DISKINSERTED
                                  : IECLASS_DISKREMOVED;
        req->io_Command = IND_WRITEEVENT;
        req->io_Data = (APTR)&event;
        req->io_Length = sizeof(event);
        DoIO((struct IORequest *)req);
        CloseDevice((struct IORequest *)req);
    }

    odfs_amiga_delete_io_request((struct IORequest *)req);
    odfs_amiga_delete_msg_port(port);
}

#if ODFS_AMIGA_OS4
static void release_vector_io_request(handler_global_t *g)
{
    if (!g)
        return;

    if (g->vector_io_req) {
        odfs_amiga_delete_io_request((struct IORequest *)g->vector_io_req);
        g->vector_io_req = NULL;
    }
    if (g->vector_io_port) {
        odfs_amiga_delete_msg_port(g->vector_io_port);
        g->vector_io_port = NULL;
    }
    g->vector_io_task = NULL;
}

static struct IOStdReq *vector_io_request_for_current_task(handler_global_t *g)
{
    struct Task *task;
    struct MsgPort *port;
    struct IOStdReq *req;

    if (!g || !g->devreq)
        return NULL;

    task = FindTask(NULL);
    if (task == g->handler_task)
        return g->devreq;

    if (g->vector_io_req && g->vector_io_task == task) {
        g->vector_io_req->io_Device = g->devreq->io_Device;
        g->vector_io_req->io_Unit = g->devreq->io_Unit;
        return g->vector_io_req;
    }

    release_vector_io_request(g);

    port = odfs_amiga_create_msg_port();
    if (!port)
        return NULL;

    req = (struct IOStdReq *)odfs_amiga_create_io_request(port, sizeof(*req));
    if (!req) {
        odfs_amiga_delete_msg_port(port);
        return NULL;
    }

    req->io_Device = g->devreq->io_Device;
    req->io_Unit = g->devreq->io_Unit;
    g->vector_io_task = task;
    g->vector_io_port = port;
    g->vector_io_req = req;
    return req;
}
#endif

/*
 * IO request the current task may wait on. In the handler task this is
 * g->devreq. On OS4, native vector callbacks run in the caller's task,
 * but g->devreq replies to the handler task's port: if a caller-task
 * DoIO() has to wait for completion, the device signals the wrong task
 * and the caller blocks forever. Such callers get a cached request with
 * a reply port owned by their own task instead. Vector callbacks are
 * serialized by fs_sem, so one cached caller-task request is enough.
 * Returns NULL when no request can be obtained.
 */
static struct IOStdReq *media_io_request(handler_global_t *g)
{
#if ODFS_AMIGA_OS4
    if (FindTask(NULL) != g->handler_task)
        return vector_io_request_for_current_task(g);
#endif
    return g->devreq;
}

/*
 * Submit an HD_SCSICMD through the current task's IO request and wait
 * for it. Returns the DoIO() result and stores the request's io_Error
 * in *io_err (IOERR_OPENFAIL when no request could be obtained).
 */
static LONG scsi_do(handler_global_t *g, struct SCSICmd *scsi, BYTE *io_err)
{
    struct IOStdReq *req = media_io_request(g);
    LONG io_rc;

    if (!req) {
        *io_err = IOERR_OPENFAIL;
        return -1;
    }

    req->io_Command = HD_SCSICMD;
    req->io_Data    = scsi;
    req->io_Length  = sizeof(*scsi);

    io_rc = DoIO((struct IORequest *)req);
    *io_err = req->io_Error;
    return io_rc;
}

static odfs_err_t amiga_read_sectors(void *ctx, uint32_t lba,
                                      uint32_t count, void *buf)
{
    amiga_media_ctx_t *am = ctx;
    handler_global_t *g = am->g;
    struct IOStdReq *req = media_io_request(g);
    uint32_t total_bytes = count * g->sector_size;
    uint8_t *out = buf;
    uint32_t done = 0;
    odfs_err_t ret = ODFS_OK;

    if (!req)
        return ODFS_ERR_NOMEM;

#if !ODFS_AMIGA_OS4
    if (amiga_can_read_direct(g, out, total_bytes, count))
        return amiga_read_direct(g, req, lba, count, total_bytes, out);
#endif

    /*
     * Read through the DMA-safe bounce buffer, one chunk at a time.
     * The bounce buffer is allocated from de_BufMemType (typically
     * MEMF_CHIP) and 16-byte aligned for 68040 DMA controllers.
     *
     * For offsets > 4GB (DVD), use TD_READ64 which splits the
     * 64-bit byte offset across io_Offset (low 32) and io_Actual
     * (high 32). CDVDFS reference: Read_From_Drive() in cdrom.c
     */
    while (done < total_bytes) {
        uint32_t chunk = total_bytes - done;
        if (chunk > g->dma_buf_size)
            chunk = g->dma_buf_size;

        uint32_t cur_lba = lba + (done / g->sector_size);
        ULONG byte_offset_lo, byte_offset_hi = 0;

        if (g->sector_size == 2048) {
            byte_offset_lo = cur_lba << 11;
            byte_offset_hi = cur_lba >> 21;
        } else {
            byte_offset_lo = cur_lba * g->sector_size;
        }

        if (byte_offset_hi != 0) {
            if (amiga_read_hi(g, req, cur_lba, chunk, g->dma_buf)
                    != ODFS_OK) {
                ret = ODFS_ERR_IO;
                goto out;
            }
            memcpy(out + done, g->dma_buf, chunk);
            done += chunk;
            continue;
        }

        req->io_Offset = byte_offset_lo;
        req->io_Actual = 0;
        req->io_Length = chunk;
        req->io_Data   = g->dma_buf;
        req->io_Command = CMD_READ;

        if (DoIO((struct IORequest *)req) != 0 ||
            req->io_Error != 0 ||
            req->io_Actual != chunk) {
            ODFS_ERROR(&g->log, ODFS_SUB_IO,
                       "sector read failed unit=%lu lba=%lu count=%lu "
                       "chunk=%lu off=%lu io_Error=%ld actual=%lu cmd=%lu",
                       (unsigned long)g->devunit,
                       (unsigned long)cur_lba,
                       (unsigned long)count,
                       (unsigned long)chunk,
                       (unsigned long)req->io_Offset,
                       (long)req->io_Error,
                       (unsigned long)req->io_Actual,
                       (unsigned long)req->io_Command);
            ret = ODFS_ERR_IO;
            goto out;
        }

        memcpy(out + done, g->dma_buf, chunk);
        done += chunk;
    }

out:
    return ret;
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

static odfs_err_t amiga_read_last_session_lba(void *ctx, uint32_t *lba_out)
{
    amiga_media_ctx_t *am = ctx;
    handler_global_t *g = am->g;
    uint8_t cmd[10];
    uint8_t buf[12];
    uint8_t sense[32];
    struct SCSICmd scsi;
    BYTE io_err;
    LONG io_rc;

    if (!lba_out)
        return ODFS_ERR_INVAL;

    *lba_out = 0;

    if (g->last_session_passthrough == 0)
        return ODFS_ERR_UNSUPPORTED;

    memset(cmd, 0, sizeof(cmd));
    memset(buf, 0, sizeof(buf));
    memset(sense, 0, sizeof(sense));

    cmd[0] = 0x43;  /* READ TOC/PMA/ATIP */
    cmd[1] = 0x00;  /* MSF=0 (LBA) */
    cmd[2] = 0x01;  /* format: multisession info */
    cmd[7] = 0x00;
    cmd[8] = sizeof(buf);

    scsi_init_read(&scsi, buf, sizeof(buf), cmd, sizeof(cmd),
                   sense, sizeof(sense));

    io_rc = scsi_do(g, &scsi, &io_err);
    if (io_rc != 0 || io_err != 0 || scsi.scsi_Status != 0) {
        if (scsi_is_unsupported_command(sense)) {
            if (g->last_session_passthrough != 0) {
                g->last_session_passthrough = 0;
                ODFS_INFO(&g->log, ODFS_SUB_MULTISESSION,
                          "READ TOC multisession info unsupported on this "
                          "device path; falling back to TOC heuristics");
            }
            return ODFS_ERR_UNSUPPORTED;
        }
        ODFS_WARN(&g->log, ODFS_SUB_MULTISESSION,
                  "READ TOC multisession info failed io_rc=%ld io_Error=%ld "
                  "scsi_Status=%lu sense=%02x/%02x/%02x",
                  (long)io_rc,
                  (long)io_err,
                  (unsigned long)scsi.scsi_Status,
                  (unsigned int)(sense[2] & 0x0f),
                  (unsigned int)sense[12],
                  (unsigned int)sense[13]);
        return ODFS_ERR_IO;
    }

    if (scsi.scsi_Actual < sizeof(buf)) {
        ODFS_WARN(&g->log, ODFS_SUB_MULTISESSION,
                  "READ TOC multisession info short response actual=%lu",
                  (unsigned long)scsi.scsi_Actual);
        return ODFS_ERR_BAD_FORMAT;
    }

    if ((buf[5] & 0x04) == 0) {
        if (g->last_session_passthrough < 1)
            g->last_session_passthrough = 1;
        return ODFS_OK;
    }

    *lba_out = ((uint32_t)buf[8] << 24) |
               ((uint32_t)buf[9] << 16) |
               ((uint32_t)buf[10] << 8) |
                (uint32_t)buf[11];

    if (g->last_session_passthrough < 1)
        g->last_session_passthrough = 1;

    return ODFS_OK;
}

#if ODFS_FEATURE_CDDA
/*
 * Read raw audio CD frames via SCSI Read CD (0xBE).
 *
 * Each audio frame is 2352 bytes of 16-bit stereo PCM at 44100Hz.
 * The Read CD command reads raw sectors without error correction
 * headers, giving us the audio data directly.
 */
static odfs_err_t amiga_read_audio(void *ctx, uint32_t lba,
                                     uint32_t count, void *buf)
{
    amiga_media_ctx_t *am = ctx;
    handler_global_t *g = am->g;
    uint8_t cmd[12];
    uint8_t sense[32];
    struct SCSICmd scsi;
    BYTE io_err;
    LONG io_rc;

    memset(cmd, 0, sizeof(cmd));
    memset(sense, 0, sizeof(sense));

    if (g->read_cd_audio == 0)
        return ODFS_ERR_UNSUPPORTED;

    /* READ CD (0xBE) CDB */
    cmd[0] = 0xBE;
    cmd[1] = 0x01 << 2;  /* expected sector type: CD-DA (audio) */
    /* starting LBA (big-endian) */
    cmd[2] = (uint8_t)(lba >> 24);
    cmd[3] = (uint8_t)(lba >> 16);
    cmd[4] = (uint8_t)(lba >> 8);
    cmd[5] = (uint8_t)(lba);
    /* transfer length in frames (big-endian, 3 bytes) */
    cmd[6] = (uint8_t)(count >> 16);
    cmd[7] = (uint8_t)(count >> 8);
    cmd[8] = (uint8_t)(count);
    cmd[9] = 0x10;  /* flags: read user data (2352 bytes per frame) */

    scsi_init_read(&scsi, buf, count * 2352, cmd, sizeof(cmd),
                   sense, sizeof(sense));

    io_rc = scsi_do(g, &scsi, &io_err);
    if (io_rc != 0 || io_err != 0 || scsi.scsi_Status != 0 ||
        scsi.scsi_Actual != scsi.scsi_Length) {
        if (scsi_is_unsupported_command(sense)) {
            if (g->read_cd_audio != 0) {
                g->read_cd_audio = 0;
                ODFS_WARN(&g->log, ODFS_SUB_CDDA,
                          "READ CD (0xBE) unsupported on this device path; "
                          "disabling CDDA audio reads");
            }
            return ODFS_ERR_UNSUPPORTED;
        }
        ODFS_ERROR(&g->log, ODFS_SUB_CDDA,
                   "READ CD (0xBE) failed io_rc=%ld io_Error=%ld "
                   "scsi_Status=%lu scsi_Actual=%lu scsi_Length=%lu "
                   "lba=%lu count=%lu sense=%02x/%02x/%02x sense_actual=%u",
                   (long)io_rc,
                   (long)io_err,
                   (unsigned long)scsi.scsi_Status,
                   (unsigned long)scsi.scsi_Actual,
                   (unsigned long)scsi.scsi_Length,
                   (unsigned long)lba,
                   (unsigned long)count,
                   (unsigned int)(sense[2] & 0x0f),
                   (unsigned int)sense[12],
                   (unsigned int)sense[13],
                   (unsigned int)scsi.scsi_SenseActual);
        return ODFS_ERR_IO;
    }

    if (g->read_cd_audio < 1)
        g->read_cd_audio = 1;

    return ODFS_OK;
}

#endif /* ODFS_FEATURE_CDDA */

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
    uint8_t sense[32];
    struct SCSICmd scsi;
    BYTE io_err;
    LONG io_rc;

    memset(toc, 0, sizeof(*toc));
    memset(cmd, 0, sizeof(cmd));
    memset(buf, 0, sizeof(buf));
    memset(sense, 0, sizeof(sense));

    if (g->toc_passthrough == 0)
        return ODFS_ERR_UNSUPPORTED;

    /* SCSI Read TOC, format 0x00 (TOC) */
    cmd[0] = 0x43;              /* READ TOC/PMA/ATIP */
    cmd[1] = 0x00;              /* MSF=0 (LBA format) */
    cmd[2] = 0x00;              /* format: TOC */
    cmd[6] = 0x01;              /* starting track */
    cmd[7] = (sizeof(buf) >> 8) & 0xFF;
    cmd[8] = sizeof(buf) & 0xFF;

    scsi_init_read(&scsi, buf, sizeof(buf), cmd, sizeof(cmd),
                   sense, sizeof(sense));

    io_rc = scsi_do(g, &scsi, &io_err);
    if (io_rc != 0 || io_err != 0 || scsi.scsi_Status != 0) {
        if (scsi_is_unsupported_command(sense)) {
            if (g->toc_passthrough != 0) {
                g->toc_passthrough = 0;
                ODFS_WARN(&g->log, ODFS_SUB_CDDA,
                          "READ TOC (0x43) unsupported on this device path; "
                          "disabling TOC-based audio features");
            }
            return ODFS_ERR_UNSUPPORTED;
        }
        ODFS_WARN(&g->log, ODFS_SUB_CDDA,
                  "READ TOC (0x43) failed io_rc=%ld io_Error=%ld "
                  "scsi_Status=%lu sense=%02x/%02x/%02x sense_actual=%u",
                  (long)io_rc,
                  (long)io_err,
                  (unsigned long)scsi.scsi_Status,
                  (unsigned int)(sense[2] & 0x0f),
                  (unsigned int)sense[12],
                  (unsigned int)sense[13],
                  (unsigned int)scsi.scsi_SenseActual);
        return ODFS_ERR_UNSUPPORTED;
    }

    /* parse TOC response */
    uint16_t toc_len = ((uint16_t)buf[0] << 8) | buf[1];
    uint8_t first_track = buf[2];
    uint8_t last_track = buf[3];
    (void)first_track;

    if (toc_len < 2) {
        ODFS_WARN(&g->log, ODFS_SUB_CDDA,
                  "READ TOC returned short header len=%u",
                  (unsigned int)toc_len);
        return ODFS_ERR_BAD_FORMAT;
    }
    if ((size_t)toc_len + 2 > sizeof(buf)) {
        ODFS_WARN(&g->log, ODFS_SUB_CDDA,
                  "READ TOC length overflow len=%u buf=%u",
                  (unsigned int)toc_len, (unsigned int)sizeof(buf));
        return ODFS_ERR_BAD_FORMAT;
    }

    /* each TOC descriptor is 8 bytes starting at offset 4 */
    int ndesc = (int)(((size_t)toc_len + 2 - 4) / 8);
    uint8_t session_count = 0;

    for (int i = 0; i < ndesc && i < 99; i++) {
        const uint8_t *desc = &buf[4 + i * 8];
        uint8_t adr_ctrl = desc[1];
        uint8_t track = desc[2];
        uint8_t control = (uint8_t)(adr_ctrl & 0x0f);
        uint32_t lba = ((uint32_t)desc[4] << 24) |
                       ((uint32_t)desc[5] << 16) |
                       ((uint32_t)desc[6] << 8)  |
                        (uint32_t)desc[7];

        if (track == 0xAA) {
            toc->leadout_lba = lba;
            continue; /* lead-out, skip */
        }

        if (session_count < 99) {
            toc->sessions[session_count].number = track;
            toc->sessions[session_count].control = control;
            toc->sessions[session_count].start_lba = lba;
            toc->sessions[session_count].length = 0;
            session_count++;
        }
    }

    for (int i = 0; i < session_count; i++) {
        uint32_t start = toc->sessions[i].start_lba;
        uint32_t end = 0;

        if (i + 1 < session_count)
            end = toc->sessions[i + 1].start_lba;
        else
            end = toc->leadout_lba;

        if (end > start)
            toc->sessions[i].length = end - start;
    }

    toc->session_count = session_count;
    toc->first_session = 1;
    toc->last_session = last_track;

    if (session_count == 0) {
        ODFS_WARN(&g->log, ODFS_SUB_CDDA,
                  "READ TOC returned no usable track descriptors");
        return ODFS_ERR_BAD_FORMAT;
    }

    if (g->toc_passthrough < 1)
        g->toc_passthrough = 1;

    return ODFS_OK;
}

#if ODFS_FEATURE_CDDA
static odfs_err_t amiga_read_cdtext(void *ctx, uint8_t **buf_out,
                                    size_t *len_out)
{
    amiga_media_ctx_t *am = ctx;
    handler_global_t *g = am->g;
    uint8_t cmd[10];
    uint8_t hdr[4];
    uint8_t sense[32];
    uint8_t *buf;
    struct SCSICmd scsi;
    uint16_t data_len;
    size_t total_len;
    BYTE io_err;
    LONG io_rc;

    if (!buf_out || !len_out)
        return ODFS_ERR_INVAL;

    *buf_out = NULL;
    *len_out = 0;

    if (g->cdtext_passthrough == 0)
        return ODFS_ERR_UNSUPPORTED;

    memset(cmd, 0, sizeof(cmd));
    memset(hdr, 0, sizeof(hdr));
    memset(sense, 0, sizeof(sense));

    cmd[0] = 0x43;  /* READ TOC/PMA/ATIP */
    cmd[1] = 0x00;  /* MSF=0 */
    cmd[2] = 0x05;  /* format: CD-Text */
    cmd[7] = 0x00;
    cmd[8] = sizeof(hdr);

    scsi_init_read(&scsi, hdr, sizeof(hdr), cmd, sizeof(cmd),
                   sense, sizeof(sense));

    io_rc = scsi_do(g, &scsi, &io_err);
    if (io_rc != 0 || io_err != 0 || scsi.scsi_Status != 0) {
        if (scsi_is_unsupported_command(sense)) {
            if (g->cdtext_passthrough != 0) {
                g->cdtext_passthrough = 0;
                ODFS_INFO(&g->log, ODFS_SUB_CDDA,
                          "READ TOC CD-Text unsupported on this device path; "
                          "disabling CD-Text queries");
            }
            return ODFS_ERR_UNSUPPORTED;
        }
        ODFS_INFO(&g->log, ODFS_SUB_CDDA,
                  "READ TOC CD-Text header unavailable io_rc=%ld "
                  "io_Error=%ld scsi_Status=%lu sense=%02x/%02x/%02x",
                  (long)io_rc,
                  (long)io_err,
                  (unsigned long)scsi.scsi_Status,
                  (unsigned int)(sense[2] & 0x0f),
                  (unsigned int)sense[12],
                  (unsigned int)sense[13]);
        return ODFS_ERR_UNSUPPORTED;
    }

    data_len = ((uint16_t)hdr[0] << 8) | hdr[1];
    total_len = (size_t)data_len + 2u;
    if (total_len <= sizeof(hdr))
        return ODFS_ERR_BAD_FORMAT;
    if (total_len > 65535u)
        return ODFS_ERR_BAD_FORMAT;

    buf = odfs_malloc(total_len);
    if (!buf)
        return ODFS_ERR_NOMEM;

    memset(cmd, 0, sizeof(cmd));
    memset(sense, 0, sizeof(sense));

    cmd[0] = 0x43;  /* READ TOC/PMA/ATIP */
    cmd[1] = 0x00;  /* MSF=0 */
    cmd[2] = 0x05;  /* format: CD-Text */
    cmd[7] = (uint8_t)(total_len >> 8);
    cmd[8] = (uint8_t)(total_len);

    scsi_init_read(&scsi, buf, (ULONG)total_len, cmd, sizeof(cmd),
                   sense, sizeof(sense));

    io_rc = scsi_do(g, &scsi, &io_err);
    if (io_rc != 0 || io_err != 0 || scsi.scsi_Status != 0 ||
        scsi.scsi_Actual < sizeof(hdr)) {
        if (scsi_is_unsupported_command(sense)) {
            if (g->cdtext_passthrough != 0) {
                g->cdtext_passthrough = 0;
                ODFS_INFO(&g->log, ODFS_SUB_CDDA,
                          "READ TOC CD-Text unsupported on this device path; "
                          "disabling CD-Text queries");
            }
            odfs_free(buf);
            return ODFS_ERR_UNSUPPORTED;
        }
        ODFS_INFO(&g->log, ODFS_SUB_CDDA,
                  "READ TOC CD-Text unavailable io_rc=%ld io_Error=%ld "
                  "scsi_Status=%lu actual=%lu sense=%02x/%02x/%02x",
                  (long)io_rc,
                  (long)io_err,
                  (unsigned long)scsi.scsi_Status,
                  (unsigned long)scsi.scsi_Actual,
                  (unsigned int)(sense[2] & 0x0f),
                  (unsigned int)sense[12],
                  (unsigned int)sense[13]);
        odfs_free(buf);
        return ODFS_ERR_UNSUPPORTED;
    }

    if (g->cdtext_passthrough < 1)
        g->cdtext_passthrough = 1;
    *buf_out = buf;
    *len_out = (size_t)scsi.scsi_Actual;
    return ODFS_OK;
}
#endif /* ODFS_FEATURE_CDDA */

/* ------------------------------------------------------------------ */
/* SCSI helper commands                                                */
/* ------------------------------------------------------------------ */

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
    BYTE io_err;
    LONG io_rc;

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

    io_rc = scsi_do(g, &scsi, &io_err);
    if (io_rc != 0 || io_err != 0 || scsi.scsi_Status != 0) {
        ODFS_WARN(&g->log, ODFS_SUB_IO,
                  "MODE SELECT failed block_length=%lu io_rc=%ld "
                  "io_Error=%ld scsi_Status=%lu",
                  (unsigned long)block_length,
                  (long)io_rc,
                  (long)io_err,
                  (unsigned long)scsi.scsi_Status);
        return 0;
    }

    return 1;
}

static const odfs_media_ops_t amiga_media_ops = {
    .read_sectors          = amiga_read_sectors,
    .sector_size           = amiga_sector_size,
    .sector_count          = amiga_sector_count,
    .read_toc              = amiga_read_toc,
    .read_last_session_lba = amiga_read_last_session_lba,
#if ODFS_FEATURE_CDDA
    /* without CDDA nothing consumes audio frames or CD-Text, and the
     * table reference would keep the SCSI pass-through code resident */
    .read_audio            = amiga_read_audio,
    .read_cdtext           = amiga_read_cdtext,
#endif
    .close                 = amiga_close,
};

/* ------------------------------------------------------------------ */
/* log sink                                                            */
/* ------------------------------------------------------------------ */

#if ODFS_SERIAL_DEBUG
#if !ODFS_AMIGA_OS4
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
#endif

#if ODFS_PACKET_TRACE
static void trace_pkt(handler_global_t *g, const char *tag, struct DosPacket *pkt)
{
    if (!pkt) {
        ODFS_TRACE(&g->log, ODFS_SUB_DOS, "%s pkt=null", tag);
        return;
    }

    ODFS_TRACE(&g->log, ODFS_SUB_DOS,
               "%s pkt=%08lx type=%ld res1=%ld res2=%ld port=%08lx "
               "link=%08lx arg1=%08lx arg2=%08lx arg3=%08lx arg4=%08lx",
               tag,
               (unsigned long)pkt,
               (long)pkt->dp_Type,
               (long)pkt->dp_Res1,
               (long)pkt->dp_Res2,
               (unsigned long)pkt->dp_Port,
               (unsigned long)pkt->dp_Link,
               (unsigned long)pkt->dp_Arg1,
               (unsigned long)pkt->dp_Arg2,
               (unsigned long)pkt->dp_Arg3,
               (unsigned long)pkt->dp_Arg4);
}

static void trace_node(handler_global_t *g, const char *tag, const odfs_node_t *node)
{
    if (!node) {
        ODFS_TRACE(&g->log, ODFS_SUB_DOS, "%s node=null", tag);
        return;
    }

    ODFS_TRACE(&g->log, ODFS_SUB_DOS,
               "%s kind=%lu backend=%lu id=%lu parent=%lu lba=%lu len=%lu "
               "size_lo=%lu name=%s",
               tag,
               (unsigned long)node->kind,
               (unsigned long)node->backend,
               (unsigned long)node->id,
               (unsigned long)node->parent_id,
               (unsigned long)node->extent.lba,
               (unsigned long)node->extent.length,
               (unsigned long)node->size,
               node->name);
}
#endif

static void log_sink(odfs_log_level_t level, odfs_log_subsys_t subsys,
                     const char *msg, void *ctx)
{
    (void)level;
    (void)subsys;
    (void)ctx;
#if ODFS_AMIGA_OS4
    DebugPrintF("%s\n", msg);
#else
    serial_puts(msg);
    raw_putchar('\n');
#endif
}
#elif ODFS_FEATURE_LOG
/* with logging compiled out entirely, nothing references a sink */
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

static struct DeviceList *volume_node_ptr(const odfs_volume_t *volume)
{
    return volume ? volume->volnode : NULL;
}

/*
 * Free-list pools for the per-packet objects. Every Lock/Open/Examine
 * allocates and frees an entry plus a lock or file handle; popping a
 * free list avoids the Forbid-protected AllocMem walk on each packet.
 * Objects are recycled per handler process and returned to the system
 * at shutdown.
 */
#define ODFS_LOCK_MAGIC 0x4f4c4b31UL /* 'OLK1' */
#define ODFS_FH_MAGIC   0x4f464831UL /* 'OFH1' */

static void *pool_pop(void **head)
{
    void *p = *head;

    if (p)
        *head = *(void **)p;
    return p;
}

static void pool_push(void **head, void *p)
{
    *(void **)p = *head;
    *head = p;
}

static void pool_drain(void **head, ULONG obj_size)
{
    void *p;

    while ((p = pool_pop(head)) != NULL)
        odfs_amiga_free_mem(p, obj_size);
}

static odfs_entry_t *alloc_entry(handler_global_t *g,
                                 odfs_volume_t *volume,
                                 const odfs_node_t *fnode,
                                 const odfs_node_t *parent,
                                 const odfs_node_t *grandparent)
{
    odfs_entry_t *entry;

    entry = pool_pop(&g->entry_pool);
    if (!entry)
        entry = odfs_amiga_alloc_mem(sizeof(*entry), MEMF_PUBLIC);
    if (!entry)
        return NULL;

    entry->volume = volume;
    entry->fnode = *fnode;
    if (parent)
        entry->parent_node = *parent;
    else
        entry->parent_node = *fnode;
    if (grandparent) {
        entry->grandparent_node = *grandparent;
        entry->has_grandparent = 1;
    } else {
        entry->grandparent_node = entry->parent_node;
        entry->has_grandparent = 0;
    }
    entry->refcount = 1;
    return entry;
}

/* pop an entry whose node fields the caller will fill in place */
static odfs_entry_t *alloc_entry_blank(handler_global_t *g,
                                       odfs_volume_t *volume)
{
    odfs_entry_t *entry;

    if (!volume)
        return NULL;
    entry = pool_pop(&g->entry_pool);
    if (!entry)
        entry = odfs_amiga_alloc_mem(sizeof(*entry), MEMF_PUBLIC);
    if (!entry)
        return NULL;
    entry->volume = volume;
    entry->has_grandparent = 0;
    entry->refcount = 1;
    return entry;
}

/*
 * alloc_entry_blank() returns NULL both when the entry pool and heap are
 * exhausted and when there is simply no mounted volume to hang the entry
 * off (g->current_volume == NULL). Callers that resolve an object from a
 * device root must not report the latter as ERROR_NO_FREE_STORE: with no
 * disc in the drive LIST/DIR CD0: would then answer "Not enough memory
 * available" even with megabytes free (issue #13). Map a missing volume to
 * the disc-state error instead, mirroring validate_object_volume().
 */
static LONG blank_entry_error(const handler_global_t *g)
{
    if (!g->current_volume)
        return g->mounted ? ERROR_DEVICE_NOT_MOUNTED : ERROR_NO_DISK;
    return ERROR_NO_FREE_STORE;
}

static odfs_entry_t *retain_entry(odfs_entry_t *entry)
{
    if (entry)
        entry->refcount++;
    return entry;
}

static void release_entry(handler_global_t *g, odfs_entry_t *entry)
{
    if (!entry)
        return;
    if (--entry->refcount == 0)
        pool_push(&g->entry_pool, entry);
}

static odfs_node_t *lock_node(odfs_lock_t *ol)
{
    return ol ? &ol->entry->fnode : NULL;
}

static odfs_node_t *lock_parent_node(odfs_lock_t *ol)
{
    return ol ? &ol->entry->parent_node : NULL;
}

static odfs_node_t *lock_grandparent_node(odfs_lock_t *ol)
{
    if (!ol || !ol->entry->has_grandparent)
        return NULL;
    return &ol->entry->grandparent_node;
}

static odfs_node_t *fh_node(odfs_fh_t *fh)
{
    return fh ? &fh->entry->fnode : NULL;
}

static odfs_volume_t *fh_volume(odfs_fh_t *fh)
{
    return fh ? fh->entry->volume : NULL;
}

/*
 * Liveness checks for caller-supplied lock and file-handle pointers.
 * The magic word is set on allocation and cleared on free, so a stale
 * or foreign pointer fails in constant time instead of walking the
 * object lists on every packet. Debug builds keep the exhaustive walk
 * to also catch objects with a valid-looking magic that were never
 * linked to this handler.
 */
static int lock_is_active(handler_global_t *g, odfs_lock_t *needle)
{
    if (!g || !needle)
        return 0;

#if ODFS_SERIAL_DEBUG
    {
        odfs_lock_t *ol;

        for (ol = (odfs_lock_t *)g->locklist.mlh_Head;
             ol->node.mln_Succ != NULL;
             ol = (odfs_lock_t *)ol->node.mln_Succ) {
            if (ol == needle)
                return needle->magic == ODFS_LOCK_MAGIC;
        }
        return 0;
    }
#else
    return needle->magic == ODFS_LOCK_MAGIC;
#endif
}

static int fh_is_active(handler_global_t *g, odfs_fh_t *needle)
{
    if (!g || !needle)
        return 0;

#if ODFS_SERIAL_DEBUG
    {
        odfs_fh_t *fh;

        for (fh = (odfs_fh_t *)g->fhlist.mlh_Head;
             fh->node.mln_Succ != NULL;
             fh = (odfs_fh_t *)fh->node.mln_Succ) {
            if (fh == needle)
                return needle->magic == ODFS_FH_MAGIC;
        }
        return 0;
    }
#else
    return needle->magic == ODFS_FH_MAGIC;
#endif
}

static odfs_volume_t *alloc_volume(handler_global_t *g, struct DeviceList *volnode)
{
    odfs_volume_t *volume;

    volume = odfs_amiga_alloc_mem(sizeof(*volume), MEMF_PUBLIC | MEMF_CLEAR);
    if (!volume)
        return NULL;

    volume->volnode = volnode;
    volume->id = g->next_volume_id++;
    AddTail((struct List *)&g->volumes, (struct Node *)&volume->node);
    return volume;
}

static void link_volume_lock(odfs_volume_t *volume, odfs_lock_t *ol)
{
    if (!volume || !ol)
        return;

    Forbid();
    ol->volume_prev = NULL;
    ol->volume_next = volume->lock_head;
    ODFS_LOCK_DOS(ol)->fl_Link = volume->lock_head ?
        LOCK_TO_BPTR(volume->lock_head) : 0;
    if (volume->lock_head)
        volume->lock_head->volume_prev = ol;
    volume->lock_head = ol;
    if (volume->volnode)
        volume->volnode->dl_LockList = LOCK_TO_BPTR(ol);
    Permit();
}

static void unlink_volume_lock(odfs_volume_t *volume, odfs_lock_t *ol)
{
    odfs_lock_t *prev;
    odfs_lock_t *next;

    if (!volume || !ol)
        return;

    Forbid();
    prev = ol->volume_prev;
    next = ol->volume_next;

    if (prev) {
        prev->volume_next = next;
        ODFS_LOCK_DOS(prev)->fl_Link = next ? LOCK_TO_BPTR(next) : 0;
    } else if (volume->lock_head == ol) {
        volume->lock_head = next;
        if (volume->volnode)
            volume->volnode->dl_LockList = next ? LOCK_TO_BPTR(next) : 0;
    }
    if (next)
        next->volume_prev = prev;

    ol->volume_prev = NULL;
    ol->volume_next = NULL;
    ODFS_LOCK_DOS(ol)->fl_Link = 0;
    Permit();
}

static int destroy_stale_volume(handler_global_t *g, odfs_volume_t *volume)
{
    if (!volume)
        return 1;

    if (volume->volnode) {
        if (!detach_volume_node(volume))
            return 0;
        destroy_volume_node(volume->volnode);
    }
    if (g->volnode == volume->volnode)
        g->volnode = NULL;
    free_volume(volume);
    return 1;
}

static void reap_stale_volumes(handler_global_t *g)
{
    odfs_volume_t *volume;
    odfs_volume_t *next;

    for (volume = (odfs_volume_t *)g->volumes.mlh_Head;
         volume->node.mln_Succ;
         volume = next) {
        next = (odfs_volume_t *)volume->node.mln_Succ;
        if (volume != g->current_volume && volume->object_count == 0)
            (void)destroy_stale_volume(g, volume);
    }
}

static void retain_volume_object(odfs_volume_t *volume)
{
    if (volume)
        volume->object_count++;
}

static void release_volume_object(handler_global_t *g, odfs_volume_t *volume)
{
    if (!volume || volume->object_count == 0)
        return;

    volume->object_count--;
    if (volume == g->current_volume)
        return;

    if (volume->object_count == 0)
        (void)destroy_stale_volume(g, volume);
}

static LONG validate_object_volume(handler_global_t *g, odfs_volume_t *volume)
{
    if (!volume)
        return g->mounted ? 0 : ERROR_NO_DISK;
    if (volume != g->current_volume)
        return ERROR_DEVICE_NOT_MOUNTED;
    return 0;
}

static int nodes_same(const odfs_node_t *a, const odfs_node_t *b)
{
    if (!a || !b)
        return 0;

    return a->kind == b->kind &&
           a->backend == b->backend &&
           a->id == b->id &&
           a->extent.lba == b->extent.lba &&
           a->extent.length == b->extent.length;
}

static int node_is_mount_root(const handler_global_t *g,
                              const odfs_node_t *fnode);

static ULONG amiga_node_key(const odfs_node_t *node)
{
    ULONG key;

    if (!node)
        return 0;

    /*
     * AmigaDOS exposes FileLock.fl_Key and FileInfoBlock.fib_DiskKey as
     * object keys.  Use stable on-disc identity rather than transient ODFS
     * node ids; ISO/Joliet nodes are regenerated during directory scans.
     */
    key = (((ULONG)node->backend & 0x7UL) << 28) |
          ((ULONG)node->extent.lba & 0x0fffffffUL);
    if ((key & 0x0fffffffUL) == 0)
        key |= ((ULONG)node->id + 1UL) & 0x0fffffffUL;
    if (key == 0)
        key = 1;
    return key;
}

static ULONG node_protection(const odfs_node_t *node)
{
    ULONG prot = 0;

    if (!node)
        return 0;

    if (node->amiga_as.has_protection) {
        prot = node->amiga_as.protection[3];
    } else if (node->mode != 0) {
        /*
         * Map POSIX (Rock Ridge PX / UDF) permissions to classic Amiga
         * bits, following the MakeCD table 6 read/execute mapping.
         *
         * The owner write and delete bits are deliberately left granted.
         * The volume is read-only and already rejects every write at the
         * packet layer, so the low-nibble RWED bits are cosmetic on the
         * disc itself; their only lasting effect is that Directory Opus and
         * "Copy CLONE" clone them onto the destination when a file is copied
         * to a writable volume.  Denying write/delete here therefore forces
         * a manual "protect +wd" on every copied file.  CD masters strip the
         * POSIX write bit unconditionally, so honouring it would mark
         * essentially every file on the disc as write- and delete-protected.
         */
        if ((node->mode & 0100) == 0)
            prot |= FIBF_EXECUTE;
        if ((node->mode & 0400) == 0)
            prot |= FIBF_READ;
#ifdef FIBF_GRP_DELETE
        if (node->mode & 0020)
            prot |= FIBF_GRP_DELETE;
#endif
#ifdef FIBF_GRP_EXECUTE
        if (node->mode & 0010)
            prot |= FIBF_GRP_EXECUTE;
#endif
#ifdef FIBF_GRP_WRITE
        if (node->mode & 0020)
            prot |= FIBF_GRP_WRITE;
#endif
#ifdef FIBF_GRP_READ
        if (node->mode & 0040)
            prot |= FIBF_GRP_READ;
#endif
#ifdef FIBF_OTR_DELETE
        if (node->mode & 0002)
            prot |= FIBF_OTR_DELETE;
#endif
#ifdef FIBF_OTR_EXECUTE
        if (node->mode & 0001)
            prot |= FIBF_OTR_EXECUTE;
#endif
#ifdef FIBF_OTR_WRITE
        if (node->mode & 0002)
            prot |= FIBF_OTR_WRITE;
#endif
#ifdef FIBF_OTR_READ
        if (node->mode & 0004)
            prot |= FIBF_OTR_READ;
#endif
    } else {
        /*
         * No permission metadata (plain ISO 9660 or UDF): present the file
         * as fully accessible (----rwed) rather than write/delete protected,
         * so copies to a writable volume need no manual "protect".  The disc
         * stays read-only through the packet layer regardless of these bits.
         */
        prot = 0;
    }

    return prot;
}

static void node_date(const odfs_node_t *node, struct DateStamp *ds)
{
    if (!ds)
        return;

    memset(ds, 0, sizeof(*ds));
    if (!node || node->mtime.year < 1978)
        return;

    if (node->mtime.month < 1 || node->mtime.month > 12 ||
        node->mtime.day < 1)
        return;

    ds->ds_Days   = days_since_1978(node->mtime.year, node->mtime.month,
                                    node->mtime.day);
    ds->ds_Minute = node->mtime.hour * 60 + node->mtime.minute;
    ds->ds_Tick   = node->mtime.second * TICKS_PER_SECOND;
}

void odfs_handler_fill_node_info(handler_global_t *g,
                                 const odfs_node_t *node,
                                 odfs_handler_node_info_t *info)
{
    if (!info)
        return;

    memset(info, 0, sizeof(*info));
    info->name = "";
    info->comment = "";

    if (!node)
        return;

    info->name = (g && node_is_mount_root(g, node)) ? g->volname : node->name;
    if (node->amiga_as.has_comment)
        info->comment = node->amiga_as.comment;
    info->key = amiga_node_key(node);
    info->protection = node_protection(node);
    info->size = node->size;
    info->is_dir = (node->kind == ODFS_NODE_DIR);
    if (info->is_dir)
        info->fib_type = ST_USERDIR;
    else if (node->kind == ODFS_NODE_SYMLINK)
        info->fib_type = ST_SOFTLINK;
    else
        info->fib_type = ST_FILE;
    if (g && node_is_mount_root(g, node))
        info->fib_type = ST_ROOT;
    node_date(node, &info->date);
}

static odfs_err_t lookup_child_node(handler_global_t *g,
                                    const odfs_node_t *dir,
                                    const char *name,
                                    odfs_node_t *out);
static odfs_err_t read_file_node(handler_global_t *g,
                                 const odfs_node_t *file,
                                 uint64_t offset,
                                 void *buf,
                                 size_t *len);

static odfs_err_t lookup_child_node(handler_global_t *g,
                                    const odfs_node_t *dir,
                                    const char *name,
                                    odfs_node_t *out)
{
#if ODFS_FEATURE_CDDA
    if (g->has_cdda && dir->backend == ODFS_BACKEND_CDDA)
        return cdda_backend_ops.lookup(g->cdda_ctx, &g->mount.cache,
                                       &g->log, dir, name, out);
#endif

    return odfs_lookup(&g->mount, dir, name, out);
}

static odfs_err_t read_file_node(handler_global_t *g,
                                 const odfs_node_t *file,
                                 uint64_t offset,
                                 void *buf,
                                 size_t *len)
{
#if ODFS_FEATURE_CDDA
    if (g->has_cdda && file->backend == ODFS_BACKEND_CDDA)
        return cdda_backend_ops.read(g->cdda_ctx, &g->mount.cache,
                                     &g->log, file, offset, buf, len);
#endif

    return odfs_read(&g->mount, file, offset, buf, len);
}

static void free_volume(odfs_volume_t *volume)
{
    if (!volume)
        return;

    Remove((struct Node *)&volume->node);
    odfs_amiga_free_mem(volume, sizeof(*volume));
}

static void mount_volume(handler_global_t *g);
static void unmount_volume(handler_global_t *g);
static int query_media_present(handler_global_t *g, ULONG *status);
static LONG probe_drive_geometry(handler_global_t *g);

static void drain_all_objects(handler_global_t *g)
{
    struct Node *node;

    while ((node = RemHead((struct List *)&g->fhlist)) != NULL) {
        odfs_fh_t *fh = (odfs_fh_t *)node;
        release_volume_object(g, fh->entry->volume);
        release_entry(g, fh->entry);
        odfs_amiga_free_mem(fh, sizeof(*fh));
    }

    while ((node = RemHead((struct List *)&g->locklist)) != NULL) {
        odfs_lock_t *ol = (odfs_lock_t *)node;
        unlink_volume_lock(ol->entry->volume, ol);
#if ODFS_AMIGA_OS4
        if (ol->lock)
            FreeDosObject(DOS_LOCK, ol->lock);
#endif
        release_volume_object(g, ol->entry->volume);
        release_entry(g, ol->entry);
        odfs_amiga_free_mem(ol, sizeof(*ol));
    }
}

/*
 * How a packet relates to mounted media:
 *   -1  not an action this handler implements
 *    0  serviceable without a mounted volume
 *    1  requires a live mount
 *
 * Unknown actions must never be failed with ERROR_NO_DISK while no
 * medium is mounted: they have to reach the dispatcher's default
 * answer, ERROR_ACTION_NOT_KNOWN. diskimage.device's MountDiskImage
 * and GUI identify drives by sending the private ACTION_GET_DISK_FSSM
 * packet and fall back to reading dn_Startup only when the handler
 * answers ERROR_ACTION_NOT_KNOWN; answering ERROR_NO_DISK made an
 * empty ODFS unit invisible to both (issue #8).
 */
static int packet_mount_need(const struct DosPacket *pkt)
{
    switch (pkt->dp_Type) {
    case ACTION_IS_FILESYSTEM:
    case ACTION_INHIBIT:
    case ACTION_DISK_INFO:
    case ACTION_INFO:
    case ACTION_FREE_LOCK:
    case ACTION_END:
    case ACTION_CURRENT_VOLUME:
    case ACTION_LOCATE_OBJECT:
    case ACTION_COPY_DIR:
    case ACTION_COPY_DIR_FH:
    case ACTION_PARENT:
    case ACTION_PARENT_FH:
    case ACTION_SAME_LOCK:
    case ACTION_EXAMINE_OBJECT:
    case ACTION_EXAMINE_NEXT:
    case ACTION_EXAMINE_FH:
    case ACTION_FINDINPUT:
    case ACTION_READ:
    case ACTION_SEEK:
    case ACTION_FH_FROM_LOCK:
    case ACTION_READ_LINK:
    case ACTION_FLUSH:
    case ACTION_MORE_CACHE:
    case ACTION_DIE:
    case ACTION_SHUTDOWN:
        return 0;
    case ACTION_EXAMINE_ALL:
    case ACTION_EXAMINE_ALL_END:
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
        return 1;
    default:
        return -1;
    }
}

/* wrap an already-populated entry in a DOS lock; consumes the entry
 * reference on success and leaves it to the caller on failure */
static odfs_lock_t *lock_from_entry(handler_global_t *g,
                                    odfs_entry_t *entry,
                                    LONG access)
{
    odfs_lock_t *ol;
    struct FileLock *lock;

    ol = pool_pop(&g->lock_pool);
    if (!ol)
        ol = odfs_amiga_alloc_mem(sizeof(*ol), MEMF_PUBLIC);
    if (!ol)
        return NULL;
    memset(ol, 0, sizeof(*ol));
#if ODFS_AMIGA_OS4
    ol->lock = AllocDosObjectTags(DOS_LOCK,
                                  ADO_DOSType, ODFS_OS4_CD_DOSTYPE,
                                  TAG_DONE);
    if (!ol->lock) {
        odfs_amiga_free_mem(ol, sizeof(*ol));
        return NULL;
    }
#endif
    ol->entry = entry;
    ol->key = amiga_node_key(&entry->fnode);
    ol->magic = ODFS_LOCK_MAGIC;
#if !ODFS_AMIGA_OS4
    ol->dos_private[0] = 0;
    ol->dos_private[1] = 0;
#endif

    lock = ODFS_LOCK_DOS(ol);
    lock->fl_Link   = 0;
    lock->fl_Key    = ol->key;
    lock->fl_Access = access;
    lock->fl_Task   = g->dosport;
    lock->fl_Volume = MKBADDR(volume_node_ptr(entry->volume));
#if ODFS_AMIGA_OS4
    lock->fl_FSPrivate1 = ol;
    lock->fl_FSPrivate2 = entry;
#endif

    retain_volume_object(entry->volume);
    AddTail((struct List *)&g->locklist, (struct Node *)&ol->node);
    link_volume_lock(entry->volume, ol);
    return ol;
}

static odfs_lock_t *alloc_lock(handler_global_t *g,
                                const odfs_node_t *fnode,
                                const odfs_node_t *parent,
                                const odfs_node_t *grandparent,
                                LONG access)
{
    odfs_entry_t *entry;
    odfs_lock_t *ol;

    if (!g->current_volume)
        return NULL;

    entry = alloc_entry(g, g->current_volume, fnode, parent, grandparent);
    if (!entry)
        return NULL;

    ol = lock_from_entry(g, entry, access);
    if (!ol)
        release_entry(g, entry);
    return ol;
}

static void free_lock(handler_global_t *g, odfs_lock_t *ol)
{
    if (!ol)
        return;
    unlink_volume_lock(ol->entry->volume, ol);
    Remove((struct Node *)&ol->node);
#if ODFS_AMIGA_OS4
    if (ol->lock)
        FreeDosObject(DOS_LOCK, ol->lock);
#endif
    release_volume_object(g, ol->entry->volume);
    release_entry(g, ol->entry);
    ol->magic = 0;
    pool_push(&g->lock_pool, ol);
}

static odfs_lock_t *dup_lock(handler_global_t *g, odfs_lock_t *src)
{
    odfs_entry_t *entry;
    odfs_lock_t *ol;

    if (!src)
        return NULL;

    entry = retain_entry(src->entry);
    ol = lock_from_entry(g, entry, ODFS_LOCK_DOS(src)->fl_Access);
    if (!ol)
        release_entry(g, entry);
    return ol;
}

/* ------------------------------------------------------------------ */
/* file handle management                                              */
/* ------------------------------------------------------------------ */

static odfs_fh_t *alloc_fh(handler_global_t *g, odfs_entry_t *entry, LONG access)
{
    odfs_fh_t *fh;

    if (!entry)
        return NULL;

    fh = pool_pop(&g->fh_pool);
    if (!fh)
        fh = odfs_amiga_alloc_mem(sizeof(*fh), MEMF_PUBLIC);
    if (!fh)
        return NULL;

    fh->entry = retain_entry(entry);
    fh->access = access;
    fh->pos = 0;
    fh->magic = ODFS_FH_MAGIC;
    retain_volume_object(entry->volume);
    AddTail((struct List *)&g->fhlist, (struct Node *)&fh->node);
    return fh;
}

static void free_fh(handler_global_t *g, odfs_fh_t *fh)
{
    if (!fh)
        return;
    Remove((struct Node *)&fh->node);
    release_volume_object(g, fh->entry->volume);
    release_entry(g, fh->entry);
    fh->magic = 0;
    pool_push(&g->fh_pool, fh);
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
 *   "//foo"      = go to parent, then descend into foo
 *   ""           = current node
 *
 * Tracks the current node, its immediate parent, and a cached parent ancestor
 * when available. When an ascent needs an unknown ancestor, reconstruct it with
 * an iterative directory walk.
 */
static void swap_node_ptrs(odfs_node_t **a, odfs_node_t **b)
{
    odfs_node_t *t = *a;

    *a = *b;
    *b = t;
}

/*
 * Details of the symbolic link that stopped a path walk, for
 * ACTION_READ_LINK: the directory holding the link, the link's name,
 * and the unconsumed remainder of the path (pointing into the caller's
 * path string, beginning with '/' or empty).
 */
typedef struct odfs_link_hit {
    odfs_node_t parent;
    char        name[256];
    const char *rest;
} odfs_link_hit_t;

static odfs_err_t resolve_amiga_path(handler_global_t *g,
                                      const odfs_node_t *start,
                                      const odfs_node_t *start_parent,
                                      const odfs_node_t *start_grandparent,
                                      const char *path,
                                      odfs_node_t *result,
                                      odfs_node_t *parent_out,
                                      odfs_node_t *grandparent_out,
                                      int *has_grandparent_out,
                                      odfs_link_hit_t *link_hit)
{
    /* Three node buffers; ascents and descents rotate the pointers
     * instead of copying nodes, so each path component moves pointers,
     * not sizeof(odfs_node_t) bytes. */
    odfs_node_t nodes[3];
    odfs_node_t *cur = &nodes[0];
    odfs_node_t *parent = &nodes[1];
    odfs_node_t *grandparent = &nodes[2];
    int has_grandparent = start_grandparent != NULL;
    const char *p = path;
    char comp[256];
    odfs_err_t err;

    *cur = *start;
    if (start_parent)
        *parent = *start_parent;
    else
        *parent = *start;
    if (start_grandparent)
        *grandparent = *start_grandparent;
    else
        *grandparent = *parent;

    if (!start_parent || node_is_mount_root(g, start)) {
        *parent = g->mount.root;
        *grandparent = g->mount.root;
        has_grandparent = 1;
#if ODFS_FEATURE_CDDA
    } else if (g->has_cdda && nodes_same(start, &g->cdda_root)) {
        *parent = g->mount.root;
        *grandparent = g->mount.root;
        has_grandparent = 1;
#endif
    }

    /* Handle colons in the path (e.g., "CD0:foo" or "LIBS:foo").
     *
     * DOS resolves device/assign prefixes before the packet reaches the
     * handler, but some callers still preserve the original colon-prefixed
     * name. Match stock filesystems and keep resolving relative to the start
     * lock DOS chose after stripping the prefix text.
     */
    const char *colon = strchr(p, ':');
    if (colon)
        p = colon + 1;

    while (*p) {
        /* "/" at current position = go to parent */
        if (*p == '/') {
            if (!node_is_mount_root(g, cur)) {
                swap_node_ptrs(&cur, &parent);
                if (node_is_mount_root(g, cur)) {
                    *parent = g->mount.root;
                    *grandparent = g->mount.root;
                    has_grandparent = 1;
#if ODFS_FEATURE_CDDA
                } else if (g->has_cdda && nodes_same(cur, &g->cdda_root)) {
                    *parent = g->mount.root;
                    *grandparent = g->mount.root;
                    has_grandparent = 1;
#endif
                } else if (has_grandparent) {
                    swap_node_ptrs(&parent, &grandparent);
                    has_grandparent = 0;
                } else {
                    err = odfs_resolve_parent_node(&g->mount, cur,
                                                   parent, grandparent);
                    if (err != ODFS_OK)
                        return err;
                    has_grandparent = 1;
                }
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
        if (cur->kind != ODFS_NODE_DIR)
            return ODFS_ERR_NOT_DIR;

#if ODFS_FEATURE_CDDA
        /* intercept "CDDA" virtual directory on mixed-mode discs */
        if (g->has_cdda && cur->extent.lba == g->mount.root.extent.lba &&
            odfs_strcasecmp(comp, "CDDA") == 0) {
            swap_node_ptrs(&grandparent, &parent);
            swap_node_ptrs(&parent, &cur);
            *cur = g->cdda_root;
            has_grandparent = 1;
            p = end;
            if (*p == '/')
                p++;
            continue;
        }
#endif

        /* rotate: grandparent := parent, parent := cur, cur := scratch */
        {
            odfs_node_t *scratch = grandparent;

            grandparent = parent;
            parent = cur;
            cur = scratch;
        }
        has_grandparent = 1;
        err = lookup_child_node(g, parent, comp, cur);
        if (err != ODFS_OK)
            return err;

        /*
         * A soft link anywhere in the path stops resolution: DOS gets
         * ERROR_IS_SOFT_LINK and reissues the request with the target
         * it obtains via ACTION_READ_LINK.
         */
        if (cur->kind == ODFS_NODE_SYMLINK) {
            if (link_hit) {
                size_t nlen = strlen(comp);

                if (nlen >= sizeof(link_hit->name))
                    nlen = sizeof(link_hit->name) - 1;
                memcpy(link_hit->name, comp, nlen);
                link_hit->name[nlen] = '\0';
                link_hit->parent = *parent;
                link_hit->rest = end;
            }
            return ODFS_ERR_IS_SYMLINK;
        }

        p = end;
        if (*p == '/')
            p++;
    }

    *result = *cur;
    *parent_out = *parent;
    if (grandparent_out)
        *grandparent_out = *grandparent;
    if (has_grandparent_out)
        *has_grandparent_out = has_grandparent;
    return ODFS_OK;
}

/* ------------------------------------------------------------------ */
/* fill FileInfoBlock from odfs_node_t                               */
/* ------------------------------------------------------------------ */

static void fill_fib(handler_global_t *g, struct FileInfoBlock *fib,
                     const odfs_node_t *fnode)
{
    odfs_handler_node_info_t info;
    int name_len;
    int comment_len;
    int max_name_len;

    memset(fib, 0, sizeof(*fib));
    odfs_handler_fill_node_info(g, fnode, &info);

    max_name_len = (info.fib_type == ST_ROOT) ? 30 : 106;
    name_len = strlen(info.name);
    if (name_len > max_name_len)
        name_len = max_name_len;
    fib->fib_FileName[0] = name_len;
    memcpy(&fib->fib_FileName[1], info.name, name_len);

    fib->fib_DirEntryType = info.fib_type;
#if !ODFS_AMIGA_OS4
    fib->fib_EntryType = fib->fib_DirEntryType;
#endif
    /* fib_Size is a signed LONG; a >=2 GiB DVD file would wrap negative
     * and make copy tools misbehave. Saturate like other filesystems. */
    fib->fib_Size = (info.size > 0x7FFFFFFFull) ? 0x7FFFFFFFl
                                                : (LONG)info.size;
    fib->fib_NumBlocks = (LONG)((info.size + 511) / 512);
    fib->fib_Protection = info.protection;
    fib->fib_Date = info.date;

    comment_len = strlen(info.comment);
    if (comment_len > (int)sizeof(fib->fib_Comment) - 1)
        comment_len = (int)sizeof(fib->fib_Comment) - 1;
    fib->fib_Comment[0] = comment_len;
    if (comment_len > 0)
        memcpy(&fib->fib_Comment[1], info.comment, comment_len);

    fib->fib_DiskKey = (LONG)info.key;
}

static int node_is_mount_root(const handler_global_t *g, const odfs_node_t *fnode)
{
    if (!g || !fnode)
        return 0;

    return odfs_node_matches_identity(fnode, &g->mount.root);
}

#if ODFS_AMIGA_OS4
static LONG resolve_object_nodes(handler_global_t *g,
                                 odfs_lock_t *parent_lock,
                                 const char *path,
                                 odfs_node_t *node_out,
                                 odfs_node_t *parent_out,
                                 odfs_node_t *grandparent_out,
                                 int *has_grandparent_out)
{
    odfs_err_t err;
    const odfs_node_t *start;
    const odfs_node_t *start_parent;
    const odfs_node_t *start_grandparent;

    if (has_grandparent_out)
        *has_grandparent_out = 0;

    if (!g || !path || !node_out || !parent_out)
        return ERROR_REQUIRED_ARG_MISSING;

    if (parent_lock) {
        LONG err_dos;

        if (!lock_is_active(g, parent_lock))
            return ERROR_INVALID_LOCK;

        err_dos = validate_object_volume(g, parent_lock->entry->volume);
        if (err_dos != 0)
            return err_dos;
        start = lock_node(parent_lock);
        start_parent = lock_parent_node(parent_lock);
        start_grandparent = lock_grandparent_node(parent_lock);
    } else {
        if (!g->mounted)
            return ERROR_NO_DISK;
        start = &g->mount.root;
        start_parent = &g->mount.root;
        start_grandparent = &g->mount.root;
    }

    err = resolve_amiga_path(g, start, start_parent, start_grandparent,
                             path, node_out, parent_out, grandparent_out,
                             has_grandparent_out, NULL);
    if (err != ODFS_OK)
        return odfs_err_to_dos(err);

    return 0;
}

LONG odfs_handler_resolve_object_node(handler_global_t *g,
                                      odfs_lock_t *parent_lock,
                                      const char *path,
                                      odfs_node_t *node_out,
                                      odfs_node_t *parent_out)
{
    return resolve_object_nodes(g, parent_lock, path, node_out, parent_out,
                                NULL, NULL);
}
#endif

/* ------------------------------------------------------------------ */
/* shared frontend operations                                          */
/* ------------------------------------------------------------------ */

/*
 * Resolve an AmigaDOS path directly into a preallocated entry's node
 * storage. The zero- and one-component forms — which cover almost
 * every Lock() and Open() — are served with a single lookup and no
 * intermediate node copies; paths with '/' fall back to the iterative
 * walker, which still writes its results straight into the entry.
 */
static LONG resolve_object_into_entry(handler_global_t *g,
                                      odfs_lock_t *parent_lock,
                                      const char *path,
                                      odfs_entry_t *entry)
{
    const odfs_node_t *start;
    const odfs_node_t *start_parent;
    const odfs_node_t *start_grandparent;
    const char *p;
    const char *colon;
    odfs_err_t err;

    if (parent_lock) {
        LONG err_dos;

        if (!lock_is_active(g, parent_lock))
            return ERROR_INVALID_LOCK;

        err_dos = validate_object_volume(g, parent_lock->entry->volume);
        if (err_dos != 0)
            return err_dos;
        start = lock_node(parent_lock);
        start_parent = lock_parent_node(parent_lock);
        start_grandparent = lock_grandparent_node(parent_lock);
    } else {
        if (!g->mounted)
            return ERROR_NO_DISK;
        start = &g->mount.root;
        start_parent = &g->mount.root;
        start_grandparent = &g->mount.root;
    }

    /* DOS strips device prefixes; tolerate callers that keep them */
    p = path;
    colon = strchr(p, ':');
    if (colon)
        p = colon + 1;

    if (strchr(p, '/') != NULL) {
        int has_grandparent = 0;

        err = resolve_amiga_path(g, start, start_parent, start_grandparent,
                                 path, &entry->fnode, &entry->parent_node,
                                 &entry->grandparent_node, &has_grandparent,
                                 NULL);
        if (err != ODFS_OK)
            return odfs_err_to_dos(err);
        entry->has_grandparent = has_grandparent;
        if (!has_grandparent)
            entry->grandparent_node = entry->parent_node;
        return 0;
    }

    if (*p == '\0') {
        /* duplicate of the starting object */
        entry->fnode = *start;
        if (!parent_lock || node_is_mount_root(g, start)) {
            entry->parent_node = g->mount.root;
            entry->grandparent_node = g->mount.root;
#if ODFS_FEATURE_CDDA
        } else if (g->has_cdda && nodes_same(start, &g->cdda_root)) {
            entry->parent_node = g->mount.root;
            entry->grandparent_node = g->mount.root;
#endif
        } else {
            entry->parent_node = *start_parent;
            entry->grandparent_node = start_grandparent ?
                *start_grandparent : entry->parent_node;
            entry->has_grandparent = start_grandparent != NULL;
            return 0;
        }
        entry->has_grandparent = 1;
        return 0;
    }

    /* single component: one lookup, parent and grandparent are known */
    if (start->kind != ODFS_NODE_DIR)
        return odfs_err_to_dos(ODFS_ERR_NOT_DIR);

#if ODFS_FEATURE_CDDA
    if (g->has_cdda && start->extent.lba == g->mount.root.extent.lba &&
        odfs_strcasecmp(p, "CDDA") == 0) {
        entry->fnode = g->cdda_root;
    } else
#endif
    {
        err = lookup_child_node(g, start, p, &entry->fnode);
        if (err != ODFS_OK)
            return odfs_err_to_dos(err);
        if (entry->fnode.kind == ODFS_NODE_SYMLINK)
            return odfs_err_to_dos(ODFS_ERR_IS_SYMLINK);
    }

    entry->parent_node = *start;
    if (!parent_lock || node_is_mount_root(g, start)) {
        entry->grandparent_node = g->mount.root;
#if ODFS_FEATURE_CDDA
    } else if (g->has_cdda && nodes_same(start, &g->cdda_root)) {
        entry->grandparent_node = g->mount.root;
#endif
    } else {
        entry->grandparent_node = start_parent ?
            *start_parent : entry->parent_node;
    }
    entry->has_grandparent = 1;
    return 0;
}

LONG odfs_handler_lock_object(handler_global_t *g,
                              odfs_lock_t *parent_lock,
                              const char *path,
                              LONG access,
                              odfs_lock_t **out)
{
    odfs_entry_t *entry;
    LONG err_dos;
    odfs_lock_t *ol;

    if (out)
        *out = NULL;
    if (!g || !path || !out)
        return ERROR_REQUIRED_ARG_MISSING;

    entry = alloc_entry_blank(g, g->current_volume);
    if (!entry)
        return blank_entry_error(g);

    err_dos = resolve_object_into_entry(g, parent_lock, path, entry);
    if (err_dos != 0) {
        release_entry(g, entry);
        return err_dos;
    }

    if (entry->fnode.kind == ODFS_NODE_DIR)
        access = SHARED_LOCK;

    ol = lock_from_entry(g, entry, access);
    if (!ol) {
        release_entry(g, entry);
        return ERROR_NO_FREE_STORE;
    }

    *out = ol;
    return 0;
}

LONG odfs_handler_free_lock_object(handler_global_t *g, odfs_lock_t *ol)
{
    if (!ol)
        return 0;
    if (!lock_is_active(g, ol))
        return ERROR_INVALID_LOCK;

    free_lock(g, ol);
    return 0;
}

LONG odfs_handler_dup_lock_object(handler_global_t *g,
                                  odfs_lock_t *src,
                                  odfs_lock_t **out)
{
    odfs_lock_t *ol;

    if (out)
        *out = NULL;
    if (!g || !out)
        return ERROR_REQUIRED_ARG_MISSING;

    if (!src) {
        if (!g->mounted)
            return ERROR_NO_DISK;
        ol = alloc_lock(g, &g->mount.root, &g->mount.root, &g->mount.root,
                        SHARED_LOCK);
    } else {
        LONG err_dos;

        if (!lock_is_active(g, src))
            return ERROR_INVALID_LOCK;

        err_dos = validate_object_volume(g, src->entry->volume);
        if (err_dos != 0)
            return err_dos;
        ol = dup_lock(g, src);
    }

    if (!ol)
        return ERROR_NO_FREE_STORE;

    *out = ol;
    return 0;
}

LONG odfs_handler_dup_lock_from_fh(handler_global_t *g,
                                   odfs_fh_t *fh,
                                   odfs_lock_t **out)
{
    odfs_lock_t *ol;

    if (out)
        *out = NULL;
    if (!g || !out)
        return ERROR_REQUIRED_ARG_MISSING;

    if (!fh) {
        if (!g->mounted)
            return ERROR_NO_DISK;
        ol = alloc_lock(g, &g->mount.root, &g->mount.root, &g->mount.root,
                        SHARED_LOCK);
    } else {
        LONG err_dos;

        if (!fh_is_active(g, fh))
            return ERROR_OBJECT_NOT_FOUND;

        err_dos = validate_object_volume(g, fh_volume(fh));
        if (err_dos != 0)
            return err_dos;
        ol = lock_from_entry(g, retain_entry(fh->entry), SHARED_LOCK);
        if (!ol)
            release_entry(g, fh->entry);
    }

    if (!ol)
        return ERROR_NO_FREE_STORE;

    *out = ol;
    return 0;
}

static LONG resolve_parent_with_cache(handler_global_t *g,
                                      const odfs_node_t *parent_node,
                                      const odfs_node_t *cached_parent,
                                      odfs_node_t *new_parent,
                                      odfs_node_t *new_grandparent,
                                      int *has_new_grandparent)
{
    odfs_err_t err;

    if (has_new_grandparent)
        *has_new_grandparent = 0;

    if (node_is_mount_root(g, parent_node)) {
        *new_parent = g->mount.root;
        *new_grandparent = g->mount.root;
        if (has_new_grandparent)
            *has_new_grandparent = 1;
        return 0;
    }

#if ODFS_FEATURE_CDDA
    if (g->has_cdda && nodes_same(parent_node, &g->cdda_root)) {
        *new_parent = g->mount.root;
        *new_grandparent = g->mount.root;
        if (has_new_grandparent)
            *has_new_grandparent = 1;
        return 0;
    }
#endif

    if (cached_parent) {
        *new_parent = *cached_parent;
        if (node_is_mount_root(g, new_parent)) {
            *new_grandparent = g->mount.root;
            if (has_new_grandparent)
                *has_new_grandparent = 1;
        } else {
            *new_grandparent = *new_parent;
        }
        return 0;
    }

    err = odfs_resolve_parent_node(&g->mount, parent_node, new_parent,
                                   new_grandparent);
    if (err != ODFS_OK)
        return odfs_err_to_dos(err);
    if (has_new_grandparent)
        *has_new_grandparent = 1;
    return 0;
}

static LONG parent_entry_object(handler_global_t *g, odfs_entry_t *entry,
                                odfs_lock_t **out)
{
    int has_new_grandparent;
    LONG err_dos;
    odfs_entry_t *pe;
    odfs_lock_t *parent;

    if (node_is_mount_root(g, &entry->fnode))
        return 0;

    pe = alloc_entry_blank(g, g->current_volume);
    if (!pe)
        return ERROR_NO_FREE_STORE;

    pe->fnode = entry->parent_node;
    err_dos = resolve_parent_with_cache(g, &entry->parent_node,
                                        entry->has_grandparent ?
                                            &entry->grandparent_node : NULL,
                                        &pe->parent_node,
                                        &pe->grandparent_node,
                                        &has_new_grandparent);
    if (err_dos != 0) {
        release_entry(g, pe);
        return err_dos;
    }
    pe->has_grandparent = has_new_grandparent;
    if (!has_new_grandparent)
        pe->grandparent_node = pe->parent_node;

    parent = lock_from_entry(g, pe, SHARED_LOCK);
    if (!parent) {
        release_entry(g, pe);
        return ERROR_NO_FREE_STORE;
    }

    *out = parent;
    return 0;
}

LONG odfs_handler_parent_lock_object(handler_global_t *g,
                                     odfs_lock_t *ol,
                                     odfs_lock_t **out)
{
    if (out)
        *out = NULL;
    if (!g || !out)
        return ERROR_REQUIRED_ARG_MISSING;

    if (!ol) {
        if (!g->mounted)
            return ERROR_NO_DISK;
        return 0;
    }

    if (!lock_is_active(g, ol))
        return ERROR_INVALID_LOCK;

    {
        LONG err_dos = validate_object_volume(g, ol->entry->volume);
        if (err_dos != 0)
            return err_dos;
    }

    return parent_entry_object(g, ol->entry, out);
}

LONG odfs_handler_parent_fh_object(handler_global_t *g,
                                   odfs_fh_t *fh,
                                   odfs_lock_t **out)
{
    if (out)
        *out = NULL;
    if (!g || !out)
        return ERROR_REQUIRED_ARG_MISSING;

    if (!fh) {
        if (!g->mounted)
            return ERROR_NO_DISK;
        return 0;
    }

    if (!fh_is_active(g, fh))
        return ERROR_OBJECT_NOT_FOUND;

    {
        LONG err_dos = validate_object_volume(g, fh_volume(fh));
        if (err_dos != 0)
            return err_dos;
    }

    return parent_entry_object(g, fh->entry, out);
}

LONG odfs_handler_same_lock_object(handler_global_t *g,
                                   odfs_lock_t *l1,
                                   odfs_lock_t *l2,
                                   LONG *same_result)
{
    odfs_volume_t *v1;
    odfs_volume_t *v2;
    int same = 0;

    if (!g || !same_result)
        return ERROR_REQUIRED_ARG_MISSING;

    *same_result = LOCK_DIFFERENT;

    if (l1 && !lock_is_active(g, l1))
        return ERROR_INVALID_LOCK;
    if (l2 && !lock_is_active(g, l2))
        return ERROR_INVALID_LOCK;

    if (!g->mounted && (!l1 || !l2))
        return ERROR_DEVICE_NOT_MOUNTED;

    v1 = l1 ? l1->entry->volume : g->current_volume;
    v2 = l2 ? l2->entry->volume : g->current_volume;
    if (v1 != v2)
        return 0;

    *same_result = LOCK_SAME_VOLUME;
    if (!l1 && !l2) {
        same = 1;
    } else if (!l1) {
        same = node_is_mount_root(g, lock_node(l2));
    } else if (!l2) {
        same = node_is_mount_root(g, lock_node(l1));
    } else {
        same = nodes_same(lock_node(l1), lock_node(l2));
    }

    if (same)
        *same_result = LOCK_SAME;
    return 0;
}

LONG odfs_handler_same_file_object(handler_global_t *g,
                                   odfs_fh_t *fh1,
                                   odfs_fh_t *fh2,
                                   LONG *same_result)
{
    odfs_volume_t *v1;
    odfs_volume_t *v2;
    int same;

    if (!g || !fh1 || !fh2 || !same_result)
        return ERROR_OBJECT_NOT_FOUND;

    *same_result = LOCK_DIFFERENT;

    if (!fh_is_active(g, fh1) || !fh_is_active(g, fh2))
        return ERROR_OBJECT_NOT_FOUND;

    v1 = fh_volume(fh1);
    v2 = fh_volume(fh2);
    if (v1 != v2)
        return 0;

    *same_result = LOCK_SAME_VOLUME;
    same = nodes_same(fh_node(fh1), fh_node(fh2));
    if (same)
        *same_result = LOCK_SAME;
    return 0;
}

LONG odfs_handler_open_object(handler_global_t *g,
                              odfs_lock_t *dirlock,
                              const char *path,
                              LONG mode,
                              odfs_fh_t **out)
{
    LONG err_dos;
    odfs_entry_t *entry;
    odfs_fh_t *fh;

    if (out)
        *out = NULL;
    if (!g || !path || !out)
        return ERROR_REQUIRED_ARG_MISSING;

    if (mode != MODE_OLDFILE)
        return ERROR_DISK_WRITE_PROTECTED;

    entry = alloc_entry_blank(g, g->current_volume);
    if (!entry)
        return blank_entry_error(g);

    err_dos = resolve_object_into_entry(g, dirlock, path, entry);
    if (err_dos != 0) {
        release_entry(g, entry);
        return err_dos;
    }

    if (entry->fnode.kind == ODFS_NODE_DIR) {
        release_entry(g, entry);
        return ERROR_OBJECT_WRONG_TYPE;
    }

    fh = alloc_fh(g, entry, SHARED_LOCK);
    release_entry(g, entry);
    if (!fh)
        return ERROR_NO_FREE_STORE;

    *out = fh;
    return 0;
}

LONG odfs_handler_open_from_lock_object(handler_global_t *g,
                                        odfs_lock_t *ol,
                                        odfs_fh_t **out)
{
    odfs_fh_t *fh;

    if (out)
        *out = NULL;
    if (!g || !ol || !out)
        return ERROR_OBJECT_NOT_FOUND;

    if (!lock_is_active(g, ol))
        return ERROR_INVALID_LOCK;

    {
        LONG err_dos = validate_object_volume(g, ol->entry->volume);
        if (err_dos != 0)
            return err_dos;
    }

    fh = alloc_fh(g, ol->entry, ODFS_LOCK_DOS(ol)->fl_Access);
    if (!fh)
        return ERROR_NO_FREE_STORE;

    free_lock(g, ol);
    *out = fh;
    return 0;
}

LONG odfs_handler_close_object(handler_global_t *g, odfs_fh_t *fh)
{
    if (!fh)
        return 0;
    if (!fh_is_active(g, fh))
        return ERROR_OBJECT_NOT_FOUND;

    free_fh(g, fh);
    return 0;
}

LONG odfs_handler_read_object(handler_global_t *g,
                              odfs_fh_t *fh,
                              void *buf,
                              LONG len,
                              LONG *actual_out)
{
    size_t actual;
    odfs_err_t err;

    if (actual_out)
        *actual_out = 0;
    if (!g || !fh || !buf || !actual_out)
        return ERROR_OBJECT_NOT_FOUND;
    if (len < 0)
        return ERROR_BAD_NUMBER;
    if (!fh_is_active(g, fh))
        return ERROR_OBJECT_NOT_FOUND;

    {
        LONG err_dos = validate_object_volume(g, fh_volume(fh));
        if (err_dos != 0)
            return err_dos;
    }

    actual = (size_t)len;
#if !ODFS_AMIGA_OS4
    {
        uint8_t *prev_buf = g->direct_read_buf;
        ULONG prev_len = g->direct_read_len;

        g->direct_read_buf = buf;
        g->direct_read_len = (ULONG)actual;
        err = read_file_node(g, fh_node(fh), fh->pos, buf, &actual);
        g->direct_read_buf = prev_buf;
        g->direct_read_len = prev_len;
    }
#else
    err = read_file_node(g, fh_node(fh), fh->pos, buf, &actual);
#endif
    if (err != ODFS_OK && actual == 0)
        return odfs_err_to_dos(err);

    fh->pos += actual;
    *actual_out = (LONG)actual;
    return 0;
}

LONG odfs_handler_seek_object(handler_global_t *g,
                              odfs_fh_t *fh,
                              int64_t offset,
                              LONG mode,
                              int64_t *oldpos_out)
{
    int64_t oldpos;
    int64_t newpos;
    uint64_t size;

    if (oldpos_out)
        *oldpos_out = -1;
    if (!g || !fh || !oldpos_out)
        return ERROR_OBJECT_NOT_FOUND;
    if (!fh_is_active(g, fh))
        return ERROR_OBJECT_NOT_FOUND;

    {
        LONG err_dos = validate_object_volume(g, fh_volume(fh));
        if (err_dos != 0)
            return err_dos;
    }

    oldpos = (int64_t)fh->pos;
    size = fh_node(fh)->size;

    switch (mode) {
    case OFFSET_BEGINNING: newpos = offset; break;
    case OFFSET_CURRENT:   newpos = oldpos + offset; break;
    case OFFSET_END:       newpos = (int64_t)size + offset; break;
    default:
        return ERROR_SEEK_ERROR;
    }

    if (newpos < 0 || (uint64_t)newpos > size)
        return ERROR_SEEK_ERROR;

    fh->pos = (uint64_t)newpos;
    *oldpos_out = oldpos;
    return 0;
}

LONG odfs_handler_get_file_position(handler_global_t *g,
                                    odfs_fh_t *fh,
                                    int64_t *pos_out)
{
    if (pos_out)
        *pos_out = -1;
    if (!g || !fh || !pos_out)
        return ERROR_OBJECT_NOT_FOUND;
    if (!fh_is_active(g, fh))
        return ERROR_OBJECT_NOT_FOUND;

    {
        LONG err_dos = validate_object_volume(g, fh_volume(fh));
        if (err_dos != 0)
            return err_dos;
    }

    *pos_out = (int64_t)fh->pos;
    return 0;
}

LONG odfs_handler_change_lock_mode(handler_global_t *g,
                                   odfs_lock_t *ol,
                                   LONG mode)
{
    if (!g || !ol)
        return ERROR_INVALID_LOCK;
    if (mode != SHARED_LOCK && mode != EXCLUSIVE_LOCK)
        return ERROR_BAD_NUMBER;
    if (!lock_is_active(g, ol))
        return ERROR_INVALID_LOCK;

    {
        LONG err_dos = validate_object_volume(g, ol->entry->volume);
        if (err_dos != 0)
            return err_dos;
    }

    ODFS_LOCK_DOS(ol)->fl_Access =
        (lock_node(ol)->kind == ODFS_NODE_DIR) ? SHARED_LOCK : mode;
    return 0;
}

LONG odfs_handler_change_file_mode(handler_global_t *g,
                                   odfs_fh_t *fh,
                                   LONG mode)
{
    if (!g || !fh)
        return ERROR_OBJECT_NOT_FOUND;
    if (mode != SHARED_LOCK && mode != EXCLUSIVE_LOCK)
        return ERROR_BAD_NUMBER;
    if (!fh_is_active(g, fh))
        return ERROR_OBJECT_NOT_FOUND;

    {
        LONG err_dos = validate_object_volume(g, fh_volume(fh));
        if (err_dos != 0)
            return err_dos;
    }

    fh->access = mode;
    return 0;
}

LONG odfs_handler_get_file_size(handler_global_t *g,
                                odfs_fh_t *fh,
                                int64_t *size_out)
{
    if (size_out)
        *size_out = -1;
    if (!g || !fh || !size_out)
        return ERROR_OBJECT_NOT_FOUND;
    if (!fh_is_active(g, fh))
        return ERROR_OBJECT_NOT_FOUND;

    {
        LONG err_dos = validate_object_volume(g, fh_volume(fh));
        if (err_dos != 0)
            return err_dos;
    }

    *size_out = (int64_t)fh_node(fh)->size;
    return 0;
}

LONG odfs_handler_fill_info(handler_global_t *g,
                            odfs_lock_t *ol,
                            struct InfoData *info)
{
    if (!g || !info)
        return ERROR_REQUIRED_ARG_MISSING;

    if (ol) {
        LONG err_dos;

        if (!lock_is_active(g, ol))
            return ERROR_INVALID_LOCK;

        err_dos = validate_object_volume(g, ol->entry->volume);
        if (err_dos != 0)
            return err_dos;
    }

    memset(info, 0, sizeof(*info));
    info->id_NumSoftErrors = 0;
    info->id_UnitNumber    = g->devunit;
    info->id_DiskState     = ID_WRITE_PROTECTED;
    info->id_NumBlocks     = g->mounted ? g->mount.total_blocks : 0;
    info->id_NumBlocksUsed = info->id_NumBlocks;
    info->id_BytesPerBlock = g->sector_size;
    info->id_DiskType      = g->mounted ? ID_DOS_DISK : ID_NO_DISK_PRESENT;
    info->id_VolumeNode    = MKBADDR(volume_node_ptr(g->current_volume));
    info->id_InUse         = (g->current_volume && g->current_volume->volnode &&
                              g->current_volume->volnode->dl_LockList) ?
                             DOSTRUE : DOSFALSE;
    return 0;
}

LONG odfs_handler_get_lock_node(handler_global_t *g,
                                odfs_lock_t *ol,
                                const odfs_node_t **node_out)
{
    if (node_out)
        *node_out = NULL;
    if (!g || !node_out)
        return ERROR_REQUIRED_ARG_MISSING;

    if (ol) {
        LONG err_dos;

        if (!lock_is_active(g, ol))
            return ERROR_INVALID_LOCK;

        err_dos = validate_object_volume(g, ol->entry->volume);
        if (err_dos != 0)
            return err_dos;
        *node_out = lock_node(ol);
    } else {
        if (!g->mounted)
            return ERROR_NO_DISK;
        *node_out = &g->mount.root;
    }

    return 0;
}

LONG odfs_handler_get_fh_node(handler_global_t *g,
                              odfs_fh_t *fh,
                              const odfs_node_t **node_out)
{
    if (node_out)
        *node_out = NULL;
    if (!g || !fh || !node_out)
        return ERROR_OBJECT_NOT_FOUND;

    if (!fh_is_active(g, fh))
        return ERROR_OBJECT_NOT_FOUND;

    {
        LONG err_dos = validate_object_volume(g, fh_volume(fh));
        if (err_dos != 0)
            return err_dos;
    }

    *node_out = fh_node(fh);
    return 0;
}

LONG odfs_handler_inhibit(handler_global_t *g, LONG state)
{
    if (!g)
        return ERROR_REQUIRED_ARG_MISSING;

    if (state != DOSFALSE) {
        g->inhibited = 1;
        unmount_volume(g);
    } else {
        g->inhibited = 0;
        mount_volume(g);
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* packet handlers                                                     */
/* ------------------------------------------------------------------ */

static void action_locate_object(handler_global_t *g, struct DosPacket *pkt)
{
    odfs_lock_t *parent_lock = LOCK_FROM_BPTR(pkt->dp_Arg1);
    LONG access = pkt->dp_Arg3;
    char path[512];
    odfs_lock_t *ol;
    LONG err_dos;

#if ODFS_SERIAL_DEBUG && ODFS_PACKET_TRACE
    trace_pkt(g, "locate-enter", pkt);
#endif
    bstr_to_cstr(pkt->dp_Arg2, path, sizeof(path));
#if ODFS_SERIAL_DEBUG && ODFS_PACKET_TRACE
    ODFS_TRACE(&g->log, ODFS_SUB_DOS, "locate-path path=%s", path);
#endif

    err_dos = odfs_handler_lock_object(g, parent_lock, path, access, &ol);
    if (err_dos != 0) {
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = err_dos;
#if ODFS_SERIAL_DEBUG && ODFS_PACKET_TRACE
        trace_pkt(g, "locate-resolve-fail", pkt);
#endif
        return;
    }

#if ODFS_SERIAL_DEBUG && ODFS_PACKET_TRACE
    trace_node(g, "locate-node", lock_node(ol));
    trace_node(g, "locate-parent", lock_parent_node(ol));
#endif

    pkt->dp_Res1 = LOCK_TO_BPTR(ol);
#if ODFS_SERIAL_DEBUG && ODFS_PACKET_TRACE
    trace_pkt(g, "locate-exit", pkt);
#endif
}

/*
 * Working set for odfs_handler_read_soft_link, heap-allocated rather
 * than placed on the stack. On OS4 the FSReadSoftLink vector runs in
 * the calling process's context, so this resolver executes on the
 * caller's stack; with ODFS_NAME_MAX at 256 the three embedded nodes
 * plus the two path buffers are ~2.9 KiB, enough to overflow a small
 * caller stack when a path crosses a symlink. Keep them off the stack,
 * matching how the lock/examine vector paths resolve into a pooled
 * entry instead of stack nodes.
 */
typedef struct readlink_scratch {
    odfs_node_t     result;
    odfs_node_t     parent;
    odfs_node_t     grandparent;
    odfs_link_hit_t hit;
    char            target[512];
    char            apath[512]; /* "amiga" is a predefined macro on m68k */
} readlink_scratch_t;

LONG odfs_handler_read_soft_link(handler_global_t *g,
                                 odfs_lock_t *parent_lock,
                                 const char *path,
                                 char *buf, LONG bufsize,
                                 LONG *err_out)
{
    const odfs_node_t *start;
    const odfs_node_t *start_parent;
    const odfs_node_t *start_grandparent;
    readlink_scratch_t *s;
    size_t alen;
    odfs_err_t err;
    LONG ret = -1;
    LONG rerr = ERROR_REQUIRED_ARG_MISSING;

    if (err_out)
        *err_out = 0;

    if (!g || !path || !buf || bufsize <= 0) {
        if (err_out)
            *err_out = ERROR_REQUIRED_ARG_MISSING;
        return -1;
    }

    if (parent_lock) {
        LONG err_dos;

        if (!lock_is_active(g, parent_lock)) {
            if (err_out)
                *err_out = ERROR_INVALID_LOCK;
            return -1;
        }
        err_dos = validate_object_volume(g, parent_lock->entry->volume);
        if (err_dos != 0) {
            if (err_out)
                *err_out = err_dos;
            return -1;
        }
        start = lock_node(parent_lock);
        start_parent = lock_parent_node(parent_lock);
        start_grandparent = lock_grandparent_node(parent_lock);
    } else {
        if (!g->mounted) {
            if (err_out)
                *err_out = ERROR_NO_DISK;
            return -1;
        }
        start = &g->mount.root;
        start_parent = &g->mount.root;
        start_grandparent = &g->mount.root;
    }

    s = odfs_amiga_alloc_mem(sizeof(*s), MEMF_PUBLIC);
    if (!s) {
        if (err_out)
            *err_out = ERROR_NO_FREE_STORE;
        return -1;
    }

    memset(&s->hit, 0, sizeof(s->hit));
    err = resolve_amiga_path(g, start, start_parent, start_grandparent,
                             path, &s->result, &s->parent, &s->grandparent,
                             NULL, &s->hit);
    if (err == ODFS_OK) {
        /* the object exists but is not a soft link */
        rerr = ERROR_OBJECT_WRONG_TYPE;
        goto out;
    }
    if (err != ODFS_ERR_IS_SYMLINK || s->hit.name[0] == '\0') {
        rerr = odfs_err_to_dos(err);
        goto out;
    }

    err = odfs_readlink(&g->mount, &s->hit.parent, s->hit.name,
                        s->target, sizeof(s->target));
    if (err != ODFS_OK) {
        rerr = odfs_err_to_dos(err);
        goto out;
    }

    if (odfs_posix_to_amiga_path(s->target, s->apath,
                                 sizeof(s->apath)) != ODFS_OK) {
        rerr = ERROR_LINE_TOO_LONG;
        goto out;
    }
    alen = strlen(s->apath);

    /* re-append what followed the link in the original path */
    if (s->hit.rest && *s->hit.rest) {
        const char *r = s->hit.rest;
        size_t rlen;

        if (*r == '/')
            r++;
        if (alen > 0 && s->apath[alen - 1] != '/' && s->apath[alen - 1] != ':') {
            if (alen + 1 >= sizeof(s->apath)) {
                rerr = ERROR_LINE_TOO_LONG;
                goto out;
            }
            s->apath[alen++] = '/';
        }
        rlen = strlen(r);
        if (alen + rlen >= sizeof(s->apath)) {
            rerr = ERROR_LINE_TOO_LONG;
            goto out;
        }
        memcpy(s->apath + alen, r, rlen + 1);
        alen += rlen;
    }

    if ((LONG)(alen + 1) > bufsize) {
        rerr = ERROR_LINE_TOO_LONG;
        ret = -2; /* buffer too small */
        goto out;
    }

    memcpy(buf, s->apath, alen + 1);
    odfs_amiga_free_mem(s, sizeof(*s));
    return (LONG)alen;

out:
    odfs_amiga_free_mem(s, sizeof(*s));
    if (err_out)
        *err_out = rerr;
    return ret;
}

/*
 * ACTION_READ_LINK: Arg1 = dir lock, Arg2 = path that stopped with
 * ERROR_IS_SOFT_LINK (a plain C string, unlike other packets), Arg3 =
 * output buffer, Arg4 = buffer size. Res1 = result length, -1 on error,
 * -2 if the buffer is too small (per the ReadLink() autodoc).
 */
static void action_read_link(handler_global_t *g, struct DosPacket *pkt)
{
    LONG err = 0;

    pkt->dp_Res1 = odfs_handler_read_soft_link(g,
                                               LOCK_FROM_BPTR(pkt->dp_Arg1),
                                               (const char *)pkt->dp_Arg2,
                                               (char *)pkt->dp_Arg3,
                                               pkt->dp_Arg4, &err);
    pkt->dp_Res2 = err;
}

static void action_free_lock(handler_global_t *g, struct DosPacket *pkt)
{
    odfs_lock_t *ol = LOCK_FROM_BPTR(pkt->dp_Arg1);
    LONG err_dos = odfs_handler_free_lock_object(g, ol);

    if (err_dos != 0) {
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = err_dos;
        return;
    }
    pkt->dp_Res1 = DOSTRUE;
}

static void action_copy_dir(handler_global_t *g, struct DosPacket *pkt)
{
    odfs_lock_t *src = LOCK_FROM_BPTR(pkt->dp_Arg1);
    odfs_lock_t *ol;
    LONG err_dos;

    err_dos = odfs_handler_dup_lock_object(g, src, &ol);
    if (err_dos != 0) {
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = err_dos;
        return;
    }
    pkt->dp_Res1 = LOCK_TO_BPTR(ol);
}

static void action_copy_dir_fh(handler_global_t *g, struct DosPacket *pkt)
{
    odfs_fh_t *fh = (odfs_fh_t *)pkt->dp_Arg1;
    odfs_lock_t *ol;
    LONG err_dos;

    err_dos = odfs_handler_dup_lock_from_fh(g, fh, &ol);
    if (err_dos != 0) {
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = err_dos;
        return;
    }
    pkt->dp_Res1 = LOCK_TO_BPTR(ol);
}

static void action_parent(handler_global_t *g, struct DosPacket *pkt)
{
    odfs_lock_t *ol = LOCK_FROM_BPTR(pkt->dp_Arg1);
    odfs_lock_t *parent;
    LONG err_dos;

#if ODFS_SERIAL_DEBUG && ODFS_PACKET_TRACE
    ODFS_TRACE(&g->log, ODFS_SUB_DOS,
               "parent-enter arg1=%08lx lock=%08lx",
               (unsigned long)pkt->dp_Arg1, (unsigned long)ol);
#endif

    err_dos = odfs_handler_parent_lock_object(g, ol, &parent);
    if (err_dos != 0) {
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = err_dos;
        return;
    }

#if ODFS_SERIAL_DEBUG && ODFS_PACKET_TRACE
    if (ol)
        trace_node(g, "parent-node", lock_node(ol));
    if (parent) {
        trace_node(g, "parent-result", lock_node(parent));
        trace_node(g, "parent-parent", lock_parent_node(parent));
    }
#endif

    pkt->dp_Res1 = LOCK_TO_BPTR(parent);
}

static void action_parent_fh(handler_global_t *g, struct DosPacket *pkt)
{
    odfs_fh_t *fh = (odfs_fh_t *)pkt->dp_Arg1;
    odfs_lock_t *parent;
    LONG err_dos;

#if ODFS_SERIAL_DEBUG && ODFS_PACKET_TRACE
    ODFS_TRACE(&g->log, ODFS_SUB_DOS,
               "parentfh-enter fh=%08lx", (unsigned long)fh);
#endif

    err_dos = odfs_handler_parent_fh_object(g, fh, &parent);
    if (err_dos != 0) {
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = err_dos;
        return;
    }

#if ODFS_SERIAL_DEBUG && ODFS_PACKET_TRACE
    if (fh)
        trace_node(g, "parentfh-node", fh_node(fh));
    if (parent)
        trace_node(g, "parentfh-result", lock_node(parent));
#endif

    pkt->dp_Res1 = LOCK_TO_BPTR(parent);
}

static void action_same_lock(handler_global_t *g, struct DosPacket *pkt)
{
    odfs_lock_t *l1 = LOCK_FROM_BPTR(pkt->dp_Arg1);
    odfs_lock_t *l2 = LOCK_FROM_BPTR(pkt->dp_Arg2);
    LONG same_result;
    LONG err_dos;

    err_dos = odfs_handler_same_lock_object(g, l1, l2, &same_result);
    if (err_dos != 0) {
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = err_dos;
        return;
    }

    pkt->dp_Res1 = (same_result == LOCK_SAME) ? DOSTRUE : DOSFALSE;
    pkt->dp_Res2 = same_result;
}

/* ---- examine ---- */

typedef struct exnext_ctx {
    handler_global_t *g;
    struct FileInfoBlock *fib;
    ULONG previous_key;
    int   first;
    int   seen_previous;
    int   found;
} exnext_ctx_t;

static odfs_err_t exnext_cb(const odfs_node_t *entry, void *ctx)
{
    exnext_ctx_t *ec = ctx;

    if (!ec->first && !ec->seen_previous) {
        if (amiga_node_key(entry) == ec->previous_key)
            ec->seen_previous = 1;
        return ODFS_OK;
    }

    fill_fib(ec->g, ec->fib, entry);
    ec->found = 1;
    return ODFS_ERR_EOF; /* stop after one entry */
}

#if !ODFS_AMIGA_OS4
static odfs_exnext_cursor_t *exnext_cursor_for(handler_global_t *g,
                                               odfs_lock_t *ol)
{
    if (ol)
        return &ol->exnext;
    return g ? &g->root_exnext : NULL;
}

static void exnext_cursor_reset(odfs_exnext_cursor_t *cursor, ULONG dir_key)
{
    if (!cursor)
        return;

    cursor->dir_key = dir_key;
    cursor->previous_key = dir_key;
    cursor->resume = 0;
    cursor->valid = 1;
    cursor->cdda_emitted = 0;
}

static void exnext_cursor_invalidate(odfs_exnext_cursor_t *cursor)
{
    if (cursor)
        cursor->valid = 0;
}

static int exnext_cursor_matches(const odfs_exnext_cursor_t *cursor,
                                 ULONG dir_key,
                                 ULONG previous_key)
{
    return cursor && cursor->valid &&
           cursor->dir_key == dir_key &&
           cursor->previous_key == previous_key;
}

static void exnext_cursor_update(odfs_exnext_cursor_t *cursor,
                                 ULONG dir_key,
                                 ULONG previous_key,
                                 uint32_t resume,
                                 int cdda_emitted)
{
    if (!cursor)
        return;

    cursor->dir_key = dir_key;
    cursor->previous_key = previous_key;
    cursor->resume = resume;
    cursor->valid = 1;
    cursor->cdda_emitted = cdda_emitted;
}
#endif

typedef struct dir_next_ctx {
    ULONG previous_key;
    int   first;
    int   seen_previous;
    int   found;
    odfs_node_t entry;
} dir_next_ctx_t;

static odfs_err_t dir_next_cb(const odfs_node_t *entry, void *ctx)
{
    dir_next_ctx_t *dc = ctx;

    if (!dc->first && !dc->seen_previous) {
        if (amiga_node_key(entry) == dc->previous_key)
            dc->seen_previous = 1;
        return ODFS_OK;
    }

    dc->entry = *entry;
    dc->found = 1;
    return ODFS_ERR_EOF;
}

LONG odfs_handler_next_dir_entry(handler_global_t *g,
                                 odfs_lock_t *ol,
                                 ULONG previous_key,
                                 uint32_t *resume_io,
                                 odfs_node_t *entry_out,
                                 ULONG *key_out)
{
    const odfs_node_t *dir;
    ULONG dir_key;
    dir_next_ctx_t dc;
    uint32_t resume = resume_io ? *resume_io : 0;
    int use_resume = (resume_io != NULL);

    if (!g || !entry_out || !key_out)
        return ERROR_REQUIRED_ARG_MISSING;

    if (ol) {
        LONG err_dos;

        if (!lock_is_active(g, ol))
            return ERROR_INVALID_LOCK;

        err_dos = validate_object_volume(g, ol->entry->volume);
        if (err_dos != 0)
            return err_dos;
        dir = lock_node(ol);
    } else if (!g->mounted) {
        return ERROR_NO_DISK;
    } else {
        dir = &g->mount.root;
    }

    if (dir->kind != ODFS_NODE_DIR)
        return ERROR_OBJECT_WRONG_TYPE;

    dir_key = ol ? ol->key : amiga_node_key(dir);
    memset(&dc, 0, sizeof(dc));
    dc.previous_key = previous_key;
    dc.first = (previous_key == 0 || previous_key == dir_key);
    if (dc.first)
        resume = 0;

#if ODFS_FEATURE_CDDA
    if (g->has_cdda && dir->backend == ODFS_BACKEND_CDDA) {
        resume = 0;
        use_resume = 0;
        if (resume_io)
            *resume_io = 0;
    }

    if (g->has_cdda && !dc.first &&
        previous_key == amiga_node_key(&g->cdda_root))
        return ERROR_NO_MORE_ENTRIES;
#endif

    if (use_resume && resume != 0)
        dc.first = 1;

#if ODFS_FEATURE_CDDA
    if (g->has_cdda && dir->backend == ODFS_BACKEND_CDDA) {
        (void)cdda_backend_ops.readdir(g->cdda_ctx, &g->mount.cache,
                                       &g->log, dir, dir_next_cb, &dc,
                                       NULL);
    } else
#endif
    {
        (void)odfs_readdir(&g->mount, dir, dir_next_cb, &dc, &resume);

#if ODFS_FEATURE_CDDA
        if (!dc.found && g->has_cdda && node_is_mount_root(g, dir) &&
            previous_key != amiga_node_key(&g->cdda_root))
            (void)dir_next_cb(&g->cdda_root, &dc);
#endif
    }

    if (!dc.found)
        return ERROR_NO_MORE_ENTRIES;

    *entry_out = dc.entry;
    *key_out = amiga_node_key(&dc.entry);
    if (use_resume)
        *resume_io = resume;
    return 0;
}

static void action_examine_object(handler_global_t *g, struct DosPacket *pkt)
{
    odfs_lock_t *ol = LOCK_FROM_BPTR(pkt->dp_Arg1);
    struct FileInfoBlock *fib = (struct FileInfoBlock *)BADDR(pkt->dp_Arg2);
    const odfs_node_t *fnode = ol ? lock_node(ol) : &g->mount.root;

    if (ol) {
        LONG err_dos = validate_object_volume(g, ol->entry->volume);
        if (err_dos != 0) {
            pkt->dp_Res1 = DOSFALSE;
            pkt->dp_Res2 = err_dos;
            return;
        }
    } else if (!g->mounted) {
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = ERROR_NO_DISK;
        return;
    }

    fill_fib(g, fib, fnode);
#if !ODFS_AMIGA_OS4
    if (fnode->kind == ODFS_NODE_DIR)
        exnext_cursor_reset(exnext_cursor_for(g, ol), amiga_node_key(fnode));
    else
        exnext_cursor_invalidate(exnext_cursor_for(g, ol));
    if (ol)
        ol->dos_private[1] = (ULONG)-1;
#endif
#if ODFS_SERIAL_DEBUG && ODFS_PACKET_TRACE
    trace_node(g, "examine-node", fnode);
    ODFS_TRACE(&g->log, ODFS_SUB_DOS,
               "examine-fib key=%08lx type=%ld size=%ld",
               (unsigned long)fib->fib_DiskKey,
               (long)fib->fib_DirEntryType,
               (long)fib->fib_Size);
#endif
    pkt->dp_Res1 = DOSTRUE;
}

static void action_examine_next(handler_global_t *g, struct DosPacket *pkt)
{
    odfs_lock_t *ol = LOCK_FROM_BPTR(pkt->dp_Arg1);
    struct FileInfoBlock *fib = (struct FileInfoBlock *)BADDR(pkt->dp_Arg2);
    const odfs_node_t *dir = ol ? lock_node(ol) : &g->mount.root;
    ULONG dir_key;
    uint32_t resume = 0;
    exnext_ctx_t ec;
#if !ODFS_AMIGA_OS4
    odfs_exnext_cursor_t *cursor = NULL;
    int use_cursor = 0;
#endif

    if (ol) {
        LONG err_dos = validate_object_volume(g, ol->entry->volume);
        if (err_dos != 0) {
            pkt->dp_Res1 = DOSFALSE;
            pkt->dp_Res2 = err_dos;
            return;
        }
    } else if (!g->mounted) {
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = ERROR_NO_DISK;
        return;
    }

    if (dir->kind != ODFS_NODE_DIR) {
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = ERROR_OBJECT_WRONG_TYPE;
        return;
    }

    dir_key = ol ? ol->key : amiga_node_key(dir);
#if !ODFS_AMIGA_OS4
    cursor = exnext_cursor_for(g, ol);
    if (dir->backend != ODFS_BACKEND_CDDA &&
        exnext_cursor_matches(cursor, dir_key, (ULONG)fib->fib_DiskKey)) {
        resume = cursor->resume;
        use_cursor = 1;
    }
#endif
    ec.g = g;
    ec.fib = fib;
    ec.previous_key = (ULONG)fib->fib_DiskKey;
    ec.first = (ec.previous_key == dir_key);
#if !ODFS_AMIGA_OS4
    if (use_cursor)
        ec.first = 1;
#endif
    ec.seen_previous = 0;
    ec.found = 0;

    /*
     * Match the Amiga CD filesystem model: Examine() leaves fib_DiskKey as
     * the directory key, and ExNext() returns each child's object key.  This
     * keeps the visible key contract while the OS3 lock-private cursor carries
     * the backend resume offset when callers preserve fib_DiskKey normally.
     */

    /* check if CDDA virtual dir was already emitted */
#if ODFS_FEATURE_CDDA
#if !ODFS_AMIGA_OS4
    if (use_cursor && cursor->cdda_emitted) {
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = ERROR_NO_MORE_ENTRIES;
        return;
    }
#endif
    if (g->has_cdda && !ec.first &&
        ec.previous_key == amiga_node_key(&g->cdda_root)) {
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
#if !ODFS_AMIGA_OS4
        if (dir->backend != ODFS_BACKEND_CDDA)
            exnext_cursor_update(cursor, dir_key, (ULONG)fib->fib_DiskKey,
                                 resume, 0);
#endif
#if ODFS_SERIAL_DEBUG && ODFS_PACKET_TRACE
        ODFS_TRACE(&g->log, ODFS_SUB_DOS,
                   "exnext-found key=%08lx type=%ld name=%s",
                   (unsigned long)fib->fib_DiskKey,
                   (long)fib->fib_DirEntryType,
                   (char *)&fib->fib_FileName[1]);
#endif
        pkt->dp_Res1 = DOSTRUE;
    } else {
#if ODFS_FEATURE_CDDA
        /* data entries exhausted — inject CDDA virtual dir if at root */
        if (g->has_cdda && node_is_mount_root(g, dir) &&
            ec.previous_key != amiga_node_key(&g->cdda_root)) {
            fill_fib(g, fib, &g->cdda_root);
#if !ODFS_AMIGA_OS4
            if (dir->backend != ODFS_BACKEND_CDDA)
                exnext_cursor_update(cursor, dir_key,
                                     (ULONG)fib->fib_DiskKey, resume, 1);
#endif
#if ODFS_SERIAL_DEBUG && ODFS_PACKET_TRACE
            ODFS_TRACE(&g->log, ODFS_SUB_DOS,
                       "exnext-inject-cdda key=%08lx",
                       (unsigned long)fib->fib_DiskKey);
#endif
            pkt->dp_Res1 = DOSTRUE;
            return;
        }
#endif
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = ERROR_NO_MORE_ENTRIES;
#if !ODFS_AMIGA_OS4
        if (dir->backend != ODFS_BACKEND_CDDA)
            exnext_cursor_update(cursor, dir_key, ec.previous_key, resume, 0);
#endif
    }
}

static size_t exall_align_size(size_t size)
{
    return (size + 1u) & ~1u;
}

static size_t exall_fixed_size(LONG data)
{
    static const size_t sizes[] = {
        0,
        8,  /* ED_NAME: ed_Next, ed_Name */
        12, /* ED_TYPE */
        16, /* ED_SIZE */
        20, /* ED_PROTECTION */
        32, /* ED_DATE */
        36, /* ED_COMMENT */
        40  /* ED_OWNER */
    };

    if (data < ED_NAME || data > ED_OWNER)
        return 0;
    return sizes[data];
}

static int exall_fill_entry(handler_global_t *g, struct ExAllData **cursor,
                            LONG *remaining, LONG data,
                            const odfs_node_t *entry)
{
    struct ExAllData *ed = *cursor;
    odfs_handler_node_info_t info;
    const char *name;
    size_t name_len;
    size_t comment_len;
    size_t need;
    size_t fixed;
    UBYTE *p;

#if ODFS_SERIAL_DEBUG && ODFS_PACKET_TRACE
    ODFS_TRACE(&g->log, ODFS_SUB_DOS,
               "exall-fill data=%ld rem=%ld kind=%lu name=%s size_lo=%lu",
               (long)data,
               (long)*remaining,
               entry ? (unsigned long)entry->kind : 0,
               entry ? entry->name : "<null>",
               entry ? (unsigned long)entry->size : 0);
#endif

    if (data <= ED_SIZE) {
        fixed = exall_fixed_size(data);
        name = (g && node_is_mount_root(g, entry)) ? g->volname :
            entry->name;
        name_len = strlen(name) + 1u;
        need = exall_align_size(fixed + name_len);

        if (need > (size_t)*remaining)
            return 0;

        memset(ed, 0, fixed);
        p = ((UBYTE *)ed) + fixed;
        ed->ed_Name = (STRPTR)p;
        memcpy(p, name, name_len);

        if (data >= ED_TYPE) {
            if (g && node_is_mount_root(g, entry))
                ed->ed_Type = ST_ROOT;
            else if (entry->kind == ODFS_NODE_DIR)
                ed->ed_Type = ST_USERDIR;
            else if (entry->kind == ODFS_NODE_SYMLINK)
                ed->ed_Type = ST_SOFTLINK;
            else
                ed->ed_Type = ST_FILE;
        }
        if (data >= ED_SIZE)
            ed->ed_Size = (entry->size > 0xFFFFFFFFull)
                              ? 0xFFFFFFFFul : (ULONG)entry->size;

        ed->ed_Next = (struct ExAllData *)(((UBYTE *)ed) + need);
        *cursor = ed->ed_Next;
        *remaining -= (LONG)need;
        return 1;
    }

    odfs_handler_fill_node_info(g, entry, &info);
    name_len = strlen(info.name) + 1u;
    comment_len = strlen(info.comment) + 1u;

    fixed = exall_fixed_size(data);
    need = fixed + name_len;
    if (data >= ED_COMMENT)
        need += comment_len;
    need = exall_align_size(need);

    if (need > (size_t)*remaining)
        return 0;

    memset(ed, 0, fixed);

    p = ((UBYTE *)ed) + fixed;
    if (data >= ED_COMMENT) {
        ed->ed_Comment = (STRPTR)p;
        memcpy(p, info.comment, comment_len);
        p += comment_len;
    }

    ed->ed_Name = (STRPTR)p;
    memcpy(p, info.name, name_len);

    if (data >= ED_TYPE)
        ed->ed_Type = info.fib_type;
    if (data >= ED_SIZE)
        ed->ed_Size = (info.size > 0xFFFFFFFFull)
                      ? 0xFFFFFFFFul : (ULONG)info.size;
    if (data >= ED_PROTECTION)
        ed->ed_Prot = info.protection;
    if (data >= ED_DATE) {
        ed->ed_Days = (ULONG)info.date.ds_Days;
        ed->ed_Mins = (ULONG)info.date.ds_Minute;
        ed->ed_Ticks = (ULONG)info.date.ds_Tick;
    }
    if (data >= ED_OWNER) {
        ed->ed_OwnerUID = 0;
        ed->ed_OwnerGID = 0;
    }

    ed->ed_Next = (struct ExAllData *)(((UBYTE *)ed) + need);
    *cursor = ed->ed_Next;
    *remaining -= (LONG)need;
    return 1;
}

typedef struct exall_ctx {
    handler_global_t *g;
    struct ExAllData *cursor;
    struct ExAllData *last;
    struct ExAllControl *control;
    LONG remaining;
    LONG data;
    ULONG previous_key;
    int seen_previous;
    int full;
} exall_ctx_t;

static odfs_err_t exall_cb(const odfs_node_t *entry, void *ctx)
{
    exall_ctx_t *ec = ctx;
    ULONG key = amiga_node_key(entry);
    struct ExAllData *slot;
    struct ExAllData *cursor_before;
    LONG remaining_before;

    if (ec->previous_key != 0 && !ec->seen_previous) {
        if (key == ec->previous_key)
            ec->seen_previous = 1;
        return ODFS_OK;
    }

    if (ec->control->eac_MatchString &&
        !MatchPatternNoCase((STRPTR)ec->control->eac_MatchString,
                            (STRPTR)entry->name))
        return ODFS_OK;

    cursor_before = ec->cursor;
    remaining_before = ec->remaining;
    slot = ec->cursor;
    if (!exall_fill_entry(ec->g, &ec->cursor, &ec->remaining, ec->data,
                          entry)) {
        ec->full = 1;
        return ODFS_ERR_EOF;
    }

    if (ec->control->eac_MatchFunc &&
        !odfs_amiga_call_hook_pkt(ec->control->eac_MatchFunc, slot,
                                  &ec->data)) {
        ec->cursor = cursor_before;
        ec->remaining = remaining_before;
        return ODFS_OK;
    }

    ec->last = slot;
    ec->control->eac_Entries++;
    ec->control->eac_LastKey = key;
    return ODFS_OK;
}

static void action_examine_all(handler_global_t *g, struct DosPacket *pkt)
{
    odfs_lock_t *ol = LOCK_FROM_BPTR(pkt->dp_Arg1);
    struct ExAllData *buf = (struct ExAllData *)pkt->dp_Arg2;
    LONG size = pkt->dp_Arg3;
    LONG data = pkt->dp_Arg4;
    struct ExAllControl *control = (struct ExAllControl *)pkt->dp_Arg5;
    const odfs_node_t *dir = ol ? lock_node(ol) : &g->mount.root;
    exall_ctx_t ec;
    uint32_t resume = 0;

    if (!buf || size <= 0 || !control) {
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = ERROR_REQUIRED_ARG_MISSING;
        return;
    }

    if (data < ED_NAME || data > ED_OWNER) {
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = ERROR_BAD_NUMBER;
        return;
    }

#if ODFS_SERIAL_DEBUG && ODFS_PACKET_TRACE
    ODFS_TRACE(&g->log, ODFS_SUB_DOS,
               "exall-enter lock=%08lx buf=%08lx size=%ld data=%ld "
               "ctrl=%08lx last=%lu match=%08lx hook=%08lx",
               (unsigned long)ol,
               (unsigned long)buf,
               (long)size,
               (long)data,
               (unsigned long)control,
               control ? (unsigned long)control->eac_LastKey : 0,
               control ? (unsigned long)control->eac_MatchString : 0,
               control ? (unsigned long)control->eac_MatchFunc : 0);
    trace_node(g, "exall-dir", dir);
#endif

    if (ol) {
        LONG err_dos = validate_object_volume(g, ol->entry->volume);
        if (err_dos != 0) {
            pkt->dp_Res1 = DOSFALSE;
            pkt->dp_Res2 = err_dos;
            return;
        }
    } else if (!g->mounted) {
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = ERROR_NO_DISK;
        return;
    }

    if (dir->kind != ODFS_NODE_DIR) {
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = ERROR_OBJECT_WRONG_TYPE;
        return;
    }

    memset(&ec, 0, sizeof(ec));
    ec.g = g;
    ec.cursor = buf;
    ec.control = control;
    ec.remaining = size;
    ec.data = data;
    ec.previous_key = control->eac_LastKey;
    ec.seen_previous = (ec.previous_key == 0);
    control->eac_Entries = 0;

#if ODFS_FEATURE_CDDA
    if (g->has_cdda && dir->backend == ODFS_BACKEND_CDDA) {
        (void)cdda_backend_ops.readdir(g->cdda_ctx, &g->mount.cache,
                                       &g->log, dir, exall_cb, &ec, &resume);
    } else
#endif
    {
        (void)odfs_readdir(&g->mount, dir, exall_cb, &ec, &resume);

#if ODFS_FEATURE_CDDA
        if (!ec.full && g->has_cdda && node_is_mount_root(g, dir) &&
            ec.previous_key != amiga_node_key(&g->cdda_root))
            (void)exall_cb(&g->cdda_root, &ec);
#endif
    }

    if (ec.last)
        ec.last->ed_Next = NULL;

    if (control->eac_Entries > 0) {
        pkt->dp_Res1 = DOSTRUE;
        return;
    }

    if (ec.full) {
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = ERROR_BUFFER_OVERFLOW;
        return;
    }

    control->eac_LastKey = 0;
    pkt->dp_Res1 = DOSFALSE;
    pkt->dp_Res2 = ERROR_NO_MORE_ENTRIES;
}

static void action_examine_all_end(handler_global_t *g __attribute__((unused)),
                                   struct DosPacket *pkt)
{
    struct ExAllControl *control = (struct ExAllControl *)pkt->dp_Arg5;

    if (control) {
        control->eac_Entries = 0;
        control->eac_LastKey = 0;
    }
    pkt->dp_Res1 = DOSTRUE;
}

static void action_examine_fh(handler_global_t *g, struct DosPacket *pkt)
{
    odfs_fh_t *fh = (odfs_fh_t *)pkt->dp_Arg1;
    struct FileInfoBlock *fib = (struct FileInfoBlock *)BADDR(pkt->dp_Arg2);

    if (!fh) {
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
        return;
    }

    if (!fh_is_active(g, fh)) {
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
        return;
    }

    {
        LONG err_dos = validate_object_volume(g, fh_volume(fh));
        if (err_dos != 0) {
            pkt->dp_Res1 = DOSFALSE;
            pkt->dp_Res2 = err_dos;
            return;
        }
    }

    fill_fib(g, fib, fh_node(fh));
    pkt->dp_Res1 = DOSTRUE;
}

static void action_fh_from_lock(handler_global_t *g, struct DosPacket *pkt)
{
    odfs_lock_t *ol = LOCK_FROM_BPTR(pkt->dp_Arg2);
    struct FileHandle *fhandle = (struct FileHandle *)BADDR(pkt->dp_Arg1);
    odfs_fh_t *fh;
    LONG err_dos;

    err_dos = odfs_handler_open_from_lock_object(g, ol, &fh);
    if (err_dos != 0) {
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = err_dos;
        return;
    }

    fhandle->fh_Arg1 = (LONG)fh;
    pkt->dp_Res1 = DOSTRUE;
}

/* ---- file I/O ---- */

static void action_findinput(handler_global_t *g, struct DosPacket *pkt)
{
    struct FileHandle *fhandle = (struct FileHandle *)BADDR(pkt->dp_Arg1);
    odfs_lock_t *dirlock = LOCK_FROM_BPTR(pkt->dp_Arg2);
    char path[512];
    odfs_fh_t *fh;
    LONG err_dos;

    bstr_to_cstr(pkt->dp_Arg3, path, sizeof(path));

    err_dos = odfs_handler_open_object(g, dirlock, path, MODE_OLDFILE, &fh);
    if (err_dos != 0) {
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = err_dos;
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
    LONG actual;
    LONG err_dos;

    err_dos = odfs_handler_read_object(g, fh, buf, len, &actual);
    if (err_dos != 0) {
        pkt->dp_Res1 = -1;
        pkt->dp_Res2 = err_dos;
        return;
    }

    pkt->dp_Res1 = actual;
}

static void action_seek(handler_global_t *g, struct DosPacket *pkt)
{
    odfs_fh_t *fh = (odfs_fh_t *)pkt->dp_Arg1;
    LONG offset = pkt->dp_Arg2;
    LONG mode = pkt->dp_Arg3;
    int64_t oldpos;
    LONG err_dos;

    err_dos = odfs_handler_seek_object(g, fh, offset, mode, &oldpos);
    if (err_dos != 0) {
        pkt->dp_Res1 = -1;
        pkt->dp_Res2 = err_dos;
        return;
    }

    pkt->dp_Res1 = (LONG)oldpos;
}

static void action_end(handler_global_t *g, struct DosPacket *pkt)
{
    odfs_fh_t *fh = (odfs_fh_t *)pkt->dp_Arg1;
    LONG err_dos;

    err_dos = odfs_handler_close_object(g, fh);
    if (err_dos != 0) {
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = err_dos;
        return;
    }

    pkt->dp_Res1 = DOSTRUE;
}

/* ---- info ---- */

static void action_disk_info(handler_global_t *g, struct DosPacket *pkt)
{
    struct InfoData *info = (struct InfoData *)BADDR(pkt->dp_Arg1);
    LONG err_dos = odfs_handler_fill_info(g, NULL, info);

    if (err_dos != 0) {
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = err_dos;
        return;
    }
    pkt->dp_Res1 = DOSTRUE;
}

static void action_info(handler_global_t *g, struct DosPacket *pkt)
{
    odfs_lock_t *ol = LOCK_FROM_BPTR(pkt->dp_Arg1);
    struct InfoData *info = (struct InfoData *)BADDR(pkt->dp_Arg2);
    LONG err_dos;

    if (pkt->dp_Arg1 && !ol) {
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = ERROR_INVALID_LOCK;
        return;
    }

    err_dos = odfs_handler_fill_info(g, ol, info);
    if (err_dos != 0) {
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = err_dos;
        return;
    }
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
    odfs_fh_t *fh = (odfs_fh_t *)pkt->dp_Arg1;

    pkt->dp_Res1 = MKBADDR(volume_node_ptr(fh ? fh_volume(fh) : g->current_volume));
    pkt->dp_Res2 = g->devunit;
}

static void action_inhibit(handler_global_t *g, struct DosPacket *pkt)
{
    LONG err_dos = odfs_handler_inhibit(g, pkt->dp_Arg1);

    if (err_dos != 0) {
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = err_dos;
        return;
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
    case ACTION_READ_LINK:      action_read_link(g, pkt); break;
    case ACTION_FREE_LOCK:      action_free_lock(g, pkt); break;
    case ACTION_COPY_DIR:       action_copy_dir(g, pkt); break;
    case ACTION_PARENT:         action_parent(g, pkt); break;
    case ACTION_SAME_LOCK:      action_same_lock(g, pkt); break;

    /* ---- examine ---- */
    case ACTION_EXAMINE_OBJECT: action_examine_object(g, pkt); break;
    case ACTION_EXAMINE_NEXT:   action_examine_next(g, pkt); break;
    case ACTION_EXAMINE_ALL:    action_examine_all(g, pkt); break;
    case ACTION_EXAMINE_ALL_END: action_examine_all_end(g, pkt); break;
    case ACTION_EXAMINE_FH:     action_examine_fh(g, pkt); break;

    /* ---- file I/O ---- */
    case ACTION_FINDINPUT:      action_findinput(g, pkt); break;
    case ACTION_READ:           action_read(g, pkt); break;
    case ACTION_SEEK:           action_seek(g, pkt); break;
    case ACTION_END:            action_end(g, pkt); break;

    /* ---- info ---- */
    case ACTION_DISK_INFO:      action_disk_info(g, pkt); break;
    case ACTION_INFO:           action_info(g, pkt); break;
    case ACTION_IS_FILESYSTEM:  action_is_filesystem(g, pkt); break;
    case ACTION_CURRENT_VOLUME: action_current_volume(g, pkt); break;
    case ACTION_INHIBIT:        action_inhibit(g, pkt); break;

    /* ---- FH variants ---- */
    case ACTION_COPY_DIR_FH:    action_copy_dir_fh(g, pkt); break;
    case ACTION_PARENT_FH:      action_parent_fh(g, pkt); break;
    case ACTION_FH_FROM_LOCK:   action_fh_from_lock(g, pkt); break;

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
    case ACTION_SHUTDOWN:
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

#if ODFS_SERIAL_DEBUG && ODFS_PACKET_TRACE
    trace_pkt(g, "return-enter", pkt);
    ODFS_TRACE(&g->log, ODFS_SUB_DOS,
               "return-enter msg=%08lx", (unsigned long)msg);
#endif
    pkt->dp_Port = g->dosport;
    msg->mn_Node.ln_Name = (char *)pkt;
    msg->mn_Node.ln_Succ = NULL;
    msg->mn_Node.ln_Pred = NULL;
#if ODFS_SERIAL_DEBUG && ODFS_PACKET_TRACE
    ODFS_TRACE(&g->log, ODFS_SUB_DOS,
               "return-putmsg reply=%08lx msg=%08lx",
               (unsigned long)replyport, (unsigned long)msg);
#endif
    PutMsg(replyport, msg);
#if ODFS_SERIAL_DEBUG && ODFS_PACKET_TRACE
    trace_pkt(g, "return-done", pkt);
#endif
}

/* ------------------------------------------------------------------ */
/* device node publication                                             */
/* ------------------------------------------------------------------ */

static void trim_trailing_colon(char *name)
{
    size_t len;

    if (!name)
        return;

    len = strlen(name);
    if (len != 0 && name[len - 1] == ':')
        name[len - 1] = '\0';
}

static int ascii_tolower_char(int ch)
{
    if (ch >= 'A' && ch <= 'Z')
        return ch + ('a' - 'A');
    return ch;
}

static int ascii_strieq(const char *a, const char *b)
{
    while (*a && *b) {
        if (ascii_tolower_char((unsigned char)*a) !=
            ascii_tolower_char((unsigned char)*b))
            return 0;
        a++;
        b++;
    }

    return *a == '\0' && *b == '\0';
}

static void device_node_name_from_bstr(BSTR bstr, char *buf, int bufsize)
{
    bstr_to_cstr(bstr, buf, bufsize);
    trim_trailing_colon(buf);
}

static int device_node_name_conflicts(handler_global_t *g, char *name,
                                      size_t name_size)
{
    struct DeviceNode *iter;
    char want[32];
    char have[32];
    ULONG flags = LDF_DEVICES | LDF_READ;
    int conflict = 0;

    if (!g || !g->devnode)
        return 0;

    device_node_name_from_bstr(g->devnode->dn_Name, want, sizeof(want));
    if (want[0] == '\0')
        return 0;

    if (name && name_size != 0) {
        size_t len = strlen(want);
        if (len >= name_size)
            len = name_size - 1;
        memcpy(name, want, len);
        name[len] = '\0';
    }

    iter = (struct DeviceNode *)AttemptLockDosList(flags);
    if (!iter)
        return 0;

    while ((iter = (struct DeviceNode *)NextDosEntry((struct DosList *)iter,
                                                     LDF_DEVICES)) != NULL) {
        if (iter == g->devnode)
            continue;

        device_node_name_from_bstr(iter->dn_Name, have, sizeof(have));
        if (ascii_strieq(have, want)) {
            conflict = 1;
            break;
        }
    }

    UnLockDosList(flags);
    return conflict;
}

static void sync_device_node(handler_global_t *g, struct DeviceNode *devnode)
{
    if (!g || !g->devnode || !devnode)
        return;

    devnode->dn_Type = DLT_DEVICE;
    devnode->dn_Task = g->dosport;
    devnode->dn_Handler = g->devnode->dn_Handler;
    devnode->dn_StackSize = g->devnode->dn_StackSize;
    devnode->dn_Priority = g->devnode->dn_Priority;
    devnode->dn_Startup = g->fssm ? MKBADDR(g->fssm) : g->devnode->dn_Startup;
    devnode->dn_SegList = g->devnode->dn_SegList;
    devnode->dn_GlobalVec = g->devnode->dn_GlobalVec;
}

static struct DeviceNode *create_device_node(handler_global_t *g)
{
    struct DeviceNode *devnode;
    char name[32];

    device_node_name_from_bstr(g->devnode ? g->devnode->dn_Name : 0,
                               name, sizeof(name));
    if (name[0] == '\0')
        memcpy(name, "ODFS0", 6);

    devnode = odfs_amiga_create_dos_entry(name, DLT_DEVICE);
    if (!devnode)
        return NULL;

#if !ODFS_AMIGA_OS4
    devnode->dn_Lock = g->devnode ? g->devnode->dn_Lock : 0;
#endif
    sync_device_node(g, devnode);

    return devnode;
}

static void destroy_device_node(struct DeviceNode *devnode)
{
    odfs_amiga_delete_dos_entry(devnode);
}

static void publish_device_node(handler_global_t *g)
{
    struct DeviceNode *iter;
    struct DeviceNode *shadow;
    char want[32];

    if (!g->devnode || g->published_devnode)
        return;

    iter = (struct DeviceNode *)AttemptLockDosList(LDF_DEVICES | LDF_WRITE);
    if (!iter) {
        ODFS_WARN(&g->log, ODFS_SUB_CORE,
                  "device node not published: devices list lock unavailable");
        return;
    }

    device_node_name_from_bstr(g->devnode->dn_Name, want, sizeof(want));

    while ((iter = (struct DeviceNode *)NextDosEntry((struct DosList *)iter,
                                                     LDF_DEVICES)) != NULL) {
        if (iter == g->devnode) {
            g->published_devnode = iter;
            break;
        }
    }

    if (g->published_devnode) {
        sync_device_node(g, g->published_devnode);
        ODFS_INFO(&g->log, ODFS_SUB_CORE,
                  "device node ready: %s (existing)",
                  want[0] ? want : "<unnamed>");
        UnLockDosList(LDF_DEVICES | LDF_WRITE);
        return;
    }

    shadow = create_device_node(g);
    if (shadow && AddDosEntry((struct DosList *)shadow)) {
        g->published_devnode = shadow;
        g->published_devnode_owned = 1;
        ODFS_INFO(&g->log, ODFS_SUB_CORE,
                  "device node ready: %s (shadow)",
                  want[0] ? want : "<unnamed>");
    } else {
        ODFS_WARN(&g->log, ODFS_SUB_CORE,
                  "device node not published: AddDosEntry failed for %s",
                  want[0] ? want : "<unnamed>");
        destroy_device_node(shadow);
    }

    UnLockDosList(LDF_DEVICES | LDF_WRITE);
}

static void unpublish_device_node(handler_global_t *g, int keep_device)
{
    struct DeviceNode *devnode;

    devnode = g->published_devnode;
    if (!devnode)
        return;

    /*
     * Detach the handler process either way: dn_Task = NULL tells DOS the
     * server is gone. With keep_device the node stays in the devices list
     * so DOS can restart the handler on the next access (ACTION_SHUTDOWN
     * with DMDF_KEEPDEVICE); otherwise remove and free the node we own.
     */
    devnode->dn_Task = NULL;

    if (g->published_devnode_owned && !keep_device) {
        if (AttemptLockDosList(LDF_DEVICES | LDF_WRITE)) {
            if (RemDosEntry((struct DosList *)devnode))
                destroy_device_node(devnode);
            UnLockDosList(LDF_DEVICES | LDF_WRITE);
        } else {
            ODFS_WARN(&g->log, ODFS_SUB_CORE,
                      "device node removal skipped: devices list lock unavailable");
        }
    }

    g->published_devnode = NULL;
    g->published_devnode_owned = 0;
}

#if ODFS_AMIGA_OS4
static LONG activate_vector_port(handler_global_t *g)
{
    struct FileSystemVectorPort *vp;
    LONG sigbit;

    if (!g || !g->process_port)
        return ERROR_REQUIRED_ARG_MISSING;

    vp = odfs_os4_alloc_vector_port(g);
    if (!vp)
        return ERROR_NO_FREE_STORE;

    sigbit = AllocSignal(-1);
    if (sigbit < 0) {
        odfs_os4_free_vector_port(vp);
        return ERROR_NO_FREE_STORE;
    }

    vp->MP.mp_Flags = PA_SIGNAL;
    vp->MP.mp_SigBit = (UBYTE)sigbit;
    vp->MP.mp_SigTask = FindTask(NULL);

    if (GetFileSystemVectorPort(&vp->MP, FS_VECTORPORT_VERSION) != vp) {
        FreeSignal(sigbit);
        odfs_os4_free_vector_port(vp);
        return ERROR_OBJECT_WRONG_TYPE;
    }

    g->vector_port = vp;
    g->vector_sigbit = sigbit;
    g->dosport = &vp->MP;
    if (g->devnode)
        g->devnode->dn_Task = g->dosport;

    ODFS_INFO(&g->log, ODFS_SUB_CORE,
              "OS4 filesystem vector port active");
    return 0;
}

static void deactivate_vector_port(handler_global_t *g)
{
    if (!g)
        return;

    if (g->vector_port) {
        odfs_os4_free_vector_port(g->vector_port);
        g->vector_port = NULL;
    }
    if (g->vector_sigbit >= 0) {
        FreeSignal((BYTE)g->vector_sigbit);
        g->vector_sigbit = -1;
    }
    if (g->process_port)
        g->dosport = g->process_port;
}
#endif

/* ------------------------------------------------------------------ */
/* volume mount / unmount                                              */
/* ------------------------------------------------------------------ */

static struct DeviceList *create_volume_node(handler_global_t *g)
{
    struct DeviceList *dl;

    dl = odfs_amiga_create_dos_entry(g->volname, DLT_VOLUME);
    if (!dl)
        return NULL;

    dl->dl_Task     = g->dosport;
    dl->dl_DiskType = ID_DOS_DISK;
    fill_volume_date(g, &dl->dl_VolumeDate);

    return dl;
}

static void destroy_volume_node(struct DeviceList *volnode)
{
    odfs_amiga_delete_dos_entry(volnode);
}

static int publish_volume_node(handler_global_t *g)
{
    struct DosList *list;
    struct DosList *iter;
    odfs_volume_t *volume;

    if (!g || !(volume = g->current_volume) || !volume->volnode)
        return 0;
    if (volume->listed)
        return 1;

    list = AttemptLockDosList(LDF_VOLUMES | LDF_WRITE);
    if (!list)
        return 0;

    /*
     * Another DOS component may have linked the node returned through
     * ACTION_DISK_INFO while our publication was pending. Recognize that
     * exact node before calling AddDosEntry() so we neither duplicate it
     * nor lose track of its DOS-list membership.
     */
    iter = list;
    while ((iter = NextDosEntry(iter, LDF_VOLUMES)) != NULL) {
        if (iter == (struct DosList *)volume->volnode) {
            volume->listed = 1;
            break;
        }
    }

    if (!volume->listed) {
        if (AddDosEntry((struct DosList *)volume->volnode)) {
            volume->listed = 1;
        } else {
#if ODFS_SERIAL_DEBUG
            ODFS_WARN(&g->log, ODFS_SUB_MOUNT,
                      "volume publication failed: AddDosEntry(%s)",
                      g->volname);
#endif
        }
    }

    UnLockDosList(LDF_VOLUMES | LDF_WRITE);
    return volume->listed;
}

static int init_volume_publish_timer(handler_global_t *g)
{
    odfs_timer_request_t *timer;

    if (g->publish_timer_open)
        return 1;

    g->publish_timer_port = odfs_amiga_create_msg_port();
    if (!g->publish_timer_port)
        return 0;

    timer = (odfs_timer_request_t *)odfs_amiga_create_io_request(
        g->publish_timer_port, sizeof(*timer));
    if (!timer)
        goto fail;

    g->publish_timer_req = ODFS_TIMER_IO(timer);
    if (OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_MICROHZ,
                   g->publish_timer_req, 0) != 0)
        goto fail;

    g->publish_timer_open = 1;
    return 1;

fail:
    if (g->publish_timer_req) {
        odfs_amiga_delete_io_request(g->publish_timer_req);
        g->publish_timer_req = NULL;
    }
    odfs_amiga_delete_msg_port(g->publish_timer_port);
    g->publish_timer_port = NULL;
    return 0;
}

static void schedule_volume_publish_retry(handler_global_t *g)
{
    odfs_timer_request_t *timer;

    if (!g || g->publish_timer_pending || !g->mounted ||
        !g->current_volume || g->current_volume->listed)
        return;

    if (!init_volume_publish_timer(g)) {
        ODFS_WARN(&g->log, ODFS_SUB_MOUNT,
                  "volume publication retry timer unavailable");
        return;
    }

    timer = (odfs_timer_request_t *)g->publish_timer_req;
    ODFS_TIMER_IO(timer)->io_Command = TR_ADDREQUEST;
    ODFS_TIMER_SECONDS(timer) = 0;
    ODFS_TIMER_MICROS(timer) = VOLUME_PUBLISH_RETRY_MICROS;
    SendIO(g->publish_timer_req);
    g->publish_timer_pending = 1;
}

static void cancel_volume_publish_retry(handler_global_t *g)
{
    if (!g || !g->publish_timer_pending)
        return;

    if (!CheckIO(g->publish_timer_req))
        AbortIO(g->publish_timer_req);
    WaitIO(g->publish_timer_req);
    g->publish_timer_pending = 0;
}

static void handle_volume_publish_retry(handler_global_t *g)
{
    if (!g || !g->publish_timer_pending ||
        !CheckIO(g->publish_timer_req))
        return;

    WaitIO(g->publish_timer_req);
    g->publish_timer_pending = 0;

    if (!g->mounted || !g->current_volume ||
        g->current_volume->listed)
        return;

    g->publish_retry_count++;

    if (publish_volume_node(g)) {
#if ODFS_SERIAL_DEBUG
        ODFS_INFO(&g->log, ODFS_SUB_MOUNT,
                  "volume published after %lu retries: %s "
                  "node=%08lx task=%08lx",
                  (unsigned long)g->publish_retry_count,
                  g->volname,
                  (unsigned long)MKBADDR(g->current_volume->volnode),
                  (unsigned long)g->current_volume->volnode->dl_Task);
#endif
        destroy_volume_publish_timer(g);
        notify_workbench_disk_change(TRUE);
    } else {
        schedule_volume_publish_retry(g);
    }
}

static void destroy_volume_publish_timer(handler_global_t *g)
{
    if (!g)
        return;

    cancel_volume_publish_retry(g);
    if (g->publish_timer_req) {
        if (g->publish_timer_open)
            CloseDevice(g->publish_timer_req);
        odfs_amiga_delete_io_request(g->publish_timer_req);
        g->publish_timer_req = NULL;
    }
    if (g->publish_timer_port) {
        odfs_amiga_delete_msg_port(g->publish_timer_port);
        g->publish_timer_port = NULL;
    }
    g->publish_timer_open = 0;
}

static int detach_volume_node(odfs_volume_t *volume)
{
    struct DeviceList *volnode;
    int removed;

    if (!volume || !volume->volnode || !volume->listed)
        return 1;

    volnode = volume->volnode;
    if (!AttemptLockDosList(LDF_VOLUMES | LDF_WRITE))
        return 0;

    removed = RemDosEntry((struct DosList *)volnode) ? 1 : 0;
    UnLockDosList(LDF_VOLUMES | LDF_WRITE);
    if (!removed)
        return 0;

    volume->listed = 0;
    volnode->dl_Task = NULL;
    return 1;
}

/* ------------------------------------------------------------------ */
/* DosEnvec control string parsing                                     */
/* ------------------------------------------------------------------ */

#if !defined(ODFS_PROFILE_ROM) || !ODFS_PROFILE_ROM
/*
 * Parse mount options from the DosEnvec de_Control BSTR field.
 * Uses ReadArgs with a template compatible with CDVDFS.
 *
 * Supported options:
 *   L=LOWERCASE/S       — force lowercase ISO names
 *   NORR=NOROCKRIDGE/S  — disable Rock Ridge
 *   NOJ=NOJOLIET/S      — disable Joliet
 *   HF=HFSFIRST/S       — prefer HFS over ISO on hybrid discs
 *   UDF/S               — prefer UDF on bridge discs
 *   FB=FILEBUFFERS/K/N  — block cache size
 *   MC=METACACHE/K/N    — parsed-directory cache budget in KiB (0 = off)
 */

#include <dos/rdargs.h>

enum {
    CTRL_LOWERCASE,
    CTRL_NOROCKRIDGE,
    CTRL_NOJOLIET,
    CTRL_HFSFIRST,
    CTRL_UDF,
    CTRL_AIFF,
    CTRL_FILEBUFFERS,
    CTRL_METACACHE,
    CTRL__COUNT
};

static void parse_control_string(handler_global_t *g __attribute__((unused)),
                                  struct DosEnvec *de,
                                  odfs_mount_opts_t *opts)
{
    STRPTR args[CTRL__COUNT];
    char buf[250];
    struct RDArgs *rdargs;
    int len, i;

    if (!de->de_Control)
        return;

    /* extract BCPL string (AROS-compatible) */
    {
        len = AROS_BSTR_strlen(de->de_Control);
        if (len <= 0 || (size_t)len >= sizeof(buf) - 1)
            return;
        memcpy(buf, AROS_BSTR_ADDR(de->de_Control), len);
        buf[len] = '\n'; /* ReadArgs needs newline terminator */
        buf[len + 1] = '\0';
    }

    /* replace '+' with space (CDVDFS convention for spaces in control field) */
    for (i = 0; i < len; i++) {
        if (buf[i] == '+') {
            if (i + 1 < len && buf[i + 1] == '+') {
                /* ++ → literal + */
                int j;
                for (j = i; j < len; j++)
                    buf[j] = buf[j + 1];
                len--;
            } else {
                buf[i] = ' ';
            }
        }
    }

    /* strip leading/trailing quotes */
    if (len > 0 && buf[0] == '"') buf[0] = ' ';
    if (len > 1 && buf[len - 1] == '"') buf[len - 1] = ' ';

    memset(args, 0, sizeof(args));

    rdargs = (struct RDArgs *)AllocDosObject(DOS_RDARGS, NULL);
    if (!rdargs)
        return;

    rdargs->RDA_Flags |= RDAF_NOPROMPT;
    rdargs->RDA_Source.CS_Buffer = (STRPTR)buf;
    rdargs->RDA_Source.CS_Length = len + 1;
    rdargs->RDA_Source.CS_CurChr = 0;

    if (ReadArgs((CONST_STRPTR)
                  "L=LOWERCASE/S,"
                  "NORR=NOROCKRIDGE/S,"
                  "NOJ=NOJOLIET/S,"
                  "HF=HFSFIRST/S,"
                  "UDF/S,"
                  "AIFF/S,"
                  "FB=FILEBUFFERS/K/N,"
                  "MC=METACACHE/K/N",
                  (LONG *)args, rdargs)) {

        if (args[CTRL_LOWERCASE])
            opts->lowercase_iso = 1;
        if (args[CTRL_NOROCKRIDGE])
            opts->disable_rr = 1;
        if (args[CTRL_NOJOLIET])
            opts->disable_joliet = 1;
        if (args[CTRL_HFSFIRST])
            opts->prefer_hfs = 1;
        if (args[CTRL_UDF])
            opts->prefer_udf = 1;
        if (args[CTRL_AIFF])
            opts->prefer_aiff = 1;
        if (args[CTRL_FILEBUFFERS])
            opts->cache_blocks = *(LONG *)args[CTRL_FILEBUFFERS];
        if (args[CTRL_METACACHE]) {
            LONG kib = *(LONG *)args[CTRL_METACACHE];

            opts->meta_cache_kib = (kib > 0) ? (uint32_t)kib : 0;
        }

        FreeArgs(rdargs);
    }

    FreeDosObject(DOS_RDARGS, rdargs);
}
#endif /* !ODFS_PROFILE_ROM */

#if ODFS_FEATURE_CDDA
static int toc_has_data_track(const odfs_toc_t *toc)
{
    uint8_t i;

    for (i = 0; i < toc->session_count; i++) {
        if ((toc->sessions[i].control & 0x04) != 0)
            return 1;
    }

    return 0;
}

static int load_cdda_disk_icon_path(cdda_context_t *ctx, const char *path)
{
    BPTR fh;
    LONG size;
    LONG actual;
    uint8_t *data;

    fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    if (!fh)
        return 0;

#if ODFS_AMIGA_OS4
    {
        int64 file_size = GetFileSize(fh);

        if (file_size <= 0 || file_size > 65536) {
            Close(fh);
            return 0;
        }
        size = (LONG)file_size;
    }
#else
    if (Seek(fh, 0, OFFSET_END) == -1) {
        Close(fh);
        return 0;
    }
    size = Seek(fh, 0, OFFSET_BEGINNING);
    if (size <= 0 || size > 65536) {
        Close(fh);
        return 0;
    }
#endif

    data = odfs_malloc((size_t)size);
    if (!data) {
        Close(fh);
        return 0;
    }

    actual = Read(fh, data, size);
    Close(fh);
    if (actual != size) {
        odfs_free(data);
        return 0;
    }

    ctx->disk_icon = data;
    ctx->disk_icon_size = (size_t)size;
    return 1;
}

static void load_cdda_disk_icon(handler_global_t *g)
{
    cdda_context_t *ctx = (cdda_context_t *)g->cdda_ctx;

    if (!ctx || ctx->is_mixed_mode)
        return;

    if (load_cdda_disk_icon_path(ctx, "ENV:Sys/def_cdda.info") ||
        load_cdda_disk_icon_path(ctx, "ENVARC:Sys/def_cdda.info")) {
        ODFS_INFO(&g->log, ODFS_SUB_MOUNT,
                  "using def_cdda.info as audio CD Disk.info");
    }
}

static void copy_pure_audio_volume_name(handler_global_t *g)
{
    const cdda_context_t *ctx = (const cdda_context_t *)g->cdda_ctx;

    if (!ctx || ctx->volume_name[0] == '\0') {
        memcpy(g->mount.volume_name, "Audio CD", 9);
        memcpy(g->volname, "Audio CD", 9);
        g->mount.volume_name[sizeof(g->mount.volume_name) - 1] = '\0';
        g->volname[sizeof(g->volname) - 1] = '\0';
        return;
    }

    memcpy(g->mount.volume_name, ctx->volume_name,
           sizeof(ctx->volume_name));
    g->mount.volume_name[sizeof(ctx->volume_name) - 1] = '\0';
    memcpy(g->volname, ctx->volume_name, sizeof(ctx->volume_name));
    g->volname[sizeof(ctx->volume_name) - 1] = '\0';
}

static void finish_pure_audio_mount(handler_global_t *g)
{
    g->has_cdda = 1;
    g->mounted = 1;
    g->mount.root = g->cdda_root;
    g->mount.backend_ops = &cdda_backend_ops;
    g->mount.backend_ctx = g->cdda_ctx;
    g->mount.active_backend = ODFS_BACKEND_CDDA;
    odfs_mount_register_backend(&g->mount, ODFS_BACKEND_CDDA,
                                &cdda_backend_ops, g->cdda_ctx,
                                &g->cdda_root);
    load_cdda_disk_icon(g);
    copy_pure_audio_volume_name(g);
    ODFS_INFO(&g->log, ODFS_SUB_MOUNT,
              "mounted pure audio CD via CDDA backend");
}
#endif

static void mount_volume(handler_global_t *g)
{
    odfs_mount_opts_t opts;
    odfs_err_t err;
    ULONG status;

    if (g->mounted || g->inhibited)
        return;

    /*
     * Don't probe an empty drive: TD_CHANGESTATE cheaply reports media
     * presence, and probing an empty unit fails a sector read for
     * every filesystem format tried before concluding "bad format"
     * (issue #8). Devices without TD_CHANGESTATE fall through to the
     * probe as before.
     */
    if (query_media_present(g, &status) && status != 0) {
        ODFS_INFO(&g->log, ODFS_SUB_MOUNT,
                  "no medium present; waiting for a disc");
        return;
    }

    /*
     * The startup probe saw an empty drive; redo geometry so drives
     * reporting non-2048 blocks get the MODE SELECT fixup before the
     * first read.
     */
    if (g->geo_pending && probe_drive_geometry(g) == 0)
        g->geo_pending = 0;

    odfs_mount_opts_default(&opts);

#if !defined(ODFS_PROFILE_ROM) || !ODFS_PROFILE_ROM
    if (g->envec)
        parse_control_string(g, g->envec, &opts);
#endif

#if ODFS_FEATURE_CDDA
    {
        odfs_toc_t toc;

        if (odfs_media_read_toc(&g->media, &toc) == ODFS_OK &&
            !toc_has_data_track(&toc)) {
            err = cdda_mount_from_toc(&toc, 0, &opts, &g->media,
                                      &g->cdda_root, &g->cdda_ctx);
            if (err != ODFS_OK) {
                ODFS_WARN(&g->log, ODFS_SUB_MOUNT,
                          "audio-only disc found no playable audio: %s",
                          odfs_err_str(err));
                return;
            }

            memset(&g->mount, 0, sizeof(g->mount));
            g->mount.media = g->media;
            g->mount.log = g->log;
            g->mount.opts = opts;
            finish_pure_audio_mount(g);
        }
    }
#endif

    if (!g->mounted) {
        err = odfs_mount(&g->media, &opts, &g->log, &g->mount);
        if (err != ODFS_OK) {
            ODFS_WARN(&g->log, ODFS_SUB_MOUNT,
                      "primary mount failed: %s",
                      odfs_err_str(err));
#if ODFS_FEATURE_CDDA
            /* no data filesystem — try pure audio CD */
            odfs_toc_t toc;
            odfs_err_t toc_err = odfs_media_read_toc(&g->media, &toc);
            odfs_err_t cdda_err = ODFS_OK;
            if (toc_err == ODFS_OK)
                cdda_err = cdda_mount_from_toc(&toc, 0, &opts, &g->media,
                                               &g->cdda_root, &g->cdda_ctx);
            if (toc_err == ODFS_OK && cdda_err == ODFS_OK) {
                finish_pure_audio_mount(g);
            } else if (toc_err != ODFS_OK) {
                ODFS_WARN(&g->log, ODFS_SUB_MOUNT,
                          "audio CD fallback failed to read TOC: %s",
                          odfs_err_str(toc_err));
            } else {
                ODFS_WARN(&g->log, ODFS_SUB_MOUNT,
                          "audio CD fallback found no playable audio: %s",
                          odfs_err_str(cdda_err));
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
                    cdda_mount_from_toc(&toc, 1, &opts, &g->media, &g->cdda_root,
                                        &g->cdda_ctx) == ODFS_OK) {
                    g->has_cdda = 1;
                    odfs_mount_register_backend(&g->mount, ODFS_BACKEND_CDDA,
                                                &cdda_backend_ops,
                                                g->cdda_ctx, &g->cdda_root);
                }
            }
#endif
        }
    }

    g->volnode = create_volume_node(g);
    if (g->volnode) {
        g->current_volume = alloc_volume(g, g->volnode);
        if (!g->current_volume) {
            destroy_volume_node(g->volnode);
            g->volnode = NULL;
            odfs_unmount(&g->mount);
            g->mounted = 0;
            return;
        }
        g->publish_retry_count = 0;
        if (publish_volume_node(g)) {
#if ODFS_SERIAL_DEBUG
            ODFS_INFO(&g->log, ODFS_SUB_MOUNT,
                      "volume published: %s node=%08lx task=%08lx",
                      g->volname,
                      (unsigned long)MKBADDR(g->volnode),
                      (unsigned long)g->volnode->dl_Task);
#endif
            notify_workbench_disk_change(TRUE);
        } else {
#if ODFS_SERIAL_DEBUG
            ODFS_WARN(&g->log, ODFS_SUB_MOUNT,
                      "volume publication deferred");
#endif
            schedule_volume_publish_retry(g);
        }
    }

}

static void unmount_volume(handler_global_t *g)
{
    odfs_volume_t *volume;

    if (!g->mounted)
        return;

    cancel_volume_publish_retry(g);
    volume = g->current_volume;

    odfs_unmount(&g->mount);
    g->mounted = 0;
    g->current_volume = NULL;
    g->volnode = NULL;

#if ODFS_FEATURE_CDDA
    g->cdda_ctx = NULL;
    g->has_cdda = 0;
#endif

    if (!volume)
        return;

    /* A listed stale volume must remain visible to DOS, but it must no
     * longer look active even if removing it has to be retried later. */
    Forbid();
    volume->volnode->dl_Task = NULL;
    Permit();

    if (volume->object_count != 0) {
        /*
         * Keep stale volumes discoverable while DOS clients still hold
         * locks or file handles. Workbench uses the retained volume node to
         * find and validate .backdrop locks after IECLASS_DISKREMOVED.
         */
        notify_workbench_disk_change(FALSE);
        return;
    }

    (void)destroy_stale_volume(g, volume);
    notify_workbench_disk_change(FALSE);
}

/* ------------------------------------------------------------------ */
/* media change detection                                              */
/* ------------------------------------------------------------------ */

static void install_media_change(handler_global_t *g)
{
    g->chgsigbit = odfs_amiga_alloc_signal(-1);
    if (g->chgsigbit == -1)
        return;

    g->chgport = odfs_amiga_create_msg_port();
    if (!g->chgport) {
        odfs_amiga_free_signal(g->chgsigbit);
        g->chgsigbit = -1;
        return;
    }

    g->chgreq = (struct IOStdReq *)odfs_amiga_create_io_request(
        g->chgport, sizeof(struct IOStdReq));
    if (!g->chgreq) {
        odfs_amiga_delete_msg_port(g->chgport);
        g->chgport = NULL;
        odfs_amiga_free_signal(g->chgsigbit);
        g->chgsigbit = -1;
        return;
    }

    /* clone the device from the main request */
    g->chgreq->io_Device = g->devreq->io_Device;
    g->chgreq->io_Unit   = g->devreq->io_Unit;

    g->chgreq->io_Command = TD_ADDCHANGEINT;
    g->changeint_data.task       = g->dosport->mp_SigTask;
    g->changeint_data.sigmask    = 1UL << g->chgsigbit;
    odfs_amiga_init_interrupt(&g->changeint, "odfs-mediachange",
                              &g->changeint_data, changeint_signal);
    g->chgreq->io_Data    = (APTR)&g->changeint;
    g->chgreq->io_Length  = sizeof(g->changeint);
    g->chgreq->io_Flags   = 0;

    SendIO((struct IORequest *)g->chgreq);
    g->chg_installed = 1;
    if (query_media_change_count(g, &g->change_count))
        g->change_count_valid = 1;
}

static void remove_media_change(handler_global_t *g)
{
    if (!g->chg_installed)
        return;

    g->chg_installed = 0;

    if (g->chgreq) {
        g->chgreq->io_Command = TD_REMCHANGEINT;
        DoIO((struct IORequest *)g->chgreq);
        /* don't CloseDevice — we don't own it */
        g->chgreq->io_Device = NULL;
        g->chgreq->io_Unit = NULL;
        odfs_amiga_delete_io_request((struct IORequest *)g->chgreq);
        g->chgreq = NULL;
    }
    if (g->chgport) {
        odfs_amiga_delete_msg_port(g->chgport);
        g->chgport = NULL;
    }
    if (g->chgsigbit != -1) {
        odfs_amiga_free_signal(g->chgsigbit);
        g->chgsigbit = -1;
    }

    g->change_count_valid = 0;
}

static int query_media_change_count(handler_global_t *g, ULONG *count)
{
    struct IOStdReq *req;

    if (!g || !g->devreq)
        return 0;
    req = media_io_request(g);
    if (!req)
        return 0;

    req->io_Command = TD_CHANGENUM;
    if (DoIO((struct IORequest *)req) != 0)
        return 0;

    if (count)
        *count = req->io_Actual;

    return 1;
}

static int query_media_present(handler_global_t *g, ULONG *status)
{
    struct IOStdReq *req;
    ULONG actual;

    if (!g || !g->devreq)
        return 0;
    req = media_io_request(g);
    if (!req)
        return 0;

    /* a driver that succeeds without writing io_Actual then reads as
     * "disk present", which falls through to the mount probe */
    req->io_Actual = 0;
    req->io_Command = TD_CHANGESTATE;
    if (DoIO((struct IORequest *)req) != 0)
        return 0;

    actual = req->io_Actual;
    if (status)
        *status = actual;

    return 1;
}

/*
 * Probe the drive geometry; when it reports a non-2048 block size,
 * switch the drive to 2048-byte CD blocks with MODE SELECT (an
 * HD_SCSICMD, issued only for a confirmed present drive so it cannot
 * hang on a phantom unit). Returns the TD_GETGEOMETRY result.
 */
static LONG probe_drive_geometry(handler_global_t *g)
{
    struct DriveGeometry geom;
    struct IOStdReq *req;
    LONG geo_rc;

    req = media_io_request(g);
    if (!req)
        return IOERR_OPENFAIL;

    memset(&geom, 0, sizeof(geom));
    req->io_Command = TD_GETGEOMETRY;
    req->io_Data    = &geom;
    req->io_Length  = sizeof(geom);
    geo_rc = DoIO((struct IORequest *)req);
    ODFS_INFO(&g->log, ODFS_SUB_IO,
              "geometry rc=%ld sector=%lu", (long)geo_rc,
              (unsigned long)geom.dg_SectorSize);

    if (geo_rc == 0 && geom.dg_SectorSize != 0 &&
        geom.dg_SectorSize != 2048) {
        ODFS_INFO(&g->log, ODFS_SUB_IO, "mode select...");
        (void)scsi_mode_select(g, 2048);
    }

    return geo_rc;
}

static void handle_media_change(handler_global_t *g)
{
    ULONG change_count;
    ULONG status;
    int count_moved = 1;    /* assume moved if the counter can't be read */

    /*
     * The change counter is only advisory. Real scsi.device/trackdisk.device
     * bump TD_CHANGENUM *before* firing the change interrupt, so an unchanged
     * counter reliably means "same disc, spurious interrupt". Poseidon's
     * usbscsi.device (massstorage.class) polls the medium from a separate
     * task and delivers the interrupt *before* it updates TD_CHANGENUM, so on
     * the first insertion after boot the counter still reads the baseline we
     * captured in install_media_change() — which used to make us swallow the
     * event and leave the disc unmounted until it was re-inserted.
     *
     * Therefore the counter must never veto a genuine present/absent
     * transition. Decide from the actual media-present state versus our own
     * mount state, and use the counter only to tell a disc *swap* apart from a
     * redundant re-interrupt on an already-mounted disc (remounting the same
     * disc would invalidate every outstanding lock and file handle).
     */
    if (query_media_change_count(g, &change_count)) {
        if (g->change_count_valid && change_count == g->change_count)
            count_moved = 0;
        g->change_count = change_count;
        g->change_count_valid = 1;
    }

    if (!query_media_present(g, &status))
        return;

    if (status != 0) {
        /* no disc present — drop the mounted volume, if any */
        unmount_volume(g);
    } else if (!g->mounted) {
        /* a disc appeared where none was mounted — mount it regardless of
         * whether the change counter has caught up yet */
        mount_volume(g);
    } else if (count_moved) {
        /* a different disc replaced the mounted one — remount */
        unmount_volume(g);
        /* re-init media adapter since cache was destroyed */
        mount_volume(g);
    }
    /* else: same disc still mounted, spurious interrupt — keep locks intact */
}

/* ------------------------------------------------------------------ */
/* handler main entry point                                            */
/* ------------------------------------------------------------------ */

void handler_main_startup(struct Message *startup_msg)
{
    handler_global_t *g;
    struct Message *msg;
    struct DosPacket *pkt;
    struct DosPacket *shutdown_pkt = NULL;
    struct FileSysStartupMsg *fssm;
    struct DosEnvec *de;
    ULONG dossig, chgsig, pubsig, waitmask, sigs;
    int running = 1;
    int keep_device;

    (void)version_string; /* ensure $VER is not optimized out */

    odfs_amiga_init_sysbase();

    g = odfs_amiga_alloc_mem(sizeof(*g), MEMF_PUBLIC | MEMF_CLEAR);
    if (!g) {
        /*
         * Never exit without answering the startup packet: DOS blocks
         * the mounting context (during boot, the whole boot) until the
         * packet is replied.
         */
        if (startup_msg && startup_msg->mn_Node.ln_Name) {
            pkt = (struct DosPacket *)startup_msg->mn_Node.ln_Name;
            if (pkt->dp_Port) {
                pkt->dp_Res1 = DOSFALSE;
                pkt->dp_Res2 = ERROR_NO_FREE_STORE;
                startup_msg->mn_Node.ln_Succ = NULL;
                startup_msg->mn_Node.ln_Pred = NULL;
                PutMsg(pkt->dp_Port, startup_msg);
            }
        }
        return;
    }

    g->sysbase = odfs_amiga_sysbase();
    g->locklist.mlh_Head     = (struct MinNode *)&g->locklist.mlh_Tail;
    g->locklist.mlh_Tail     = NULL;
    g->locklist.mlh_TailPred = (struct MinNode *)&g->locklist.mlh_Head;
    g->fhlist.mlh_Head       = (struct MinNode *)&g->fhlist.mlh_Tail;
    g->fhlist.mlh_Tail       = NULL;
    g->fhlist.mlh_TailPred   = (struct MinNode *)&g->fhlist.mlh_Head;
    g->volumes.mlh_Head      = (struct MinNode *)&g->volumes.mlh_Tail;
    g->volumes.mlh_Tail      = NULL;
    g->volumes.mlh_TailPred  = (struct MinNode *)&g->volumes.mlh_Head;
    g->next_volume_id = 1;
#if ODFS_AMIGA_OS4
    g->vector_sigbit = -1;
    InitSemaphore(&g->fs_sem);
#endif
    g->chgsigbit = -1;
    g->toc_passthrough = -1;
    g->last_session_passthrough = -1;
    g->read_cd_audio = -1;
    g->cdtext_passthrough = -1;

    {
        struct Process *proc = (struct Process *)FindTask(NULL);
#if ODFS_AMIGA_OS4
        g->handler_task = (struct Task *)proc;
#endif
        g->process_port = &proc->pr_MsgPort;
        g->dosport = g->process_port;
    }

    /* wait for startup packet */
    if (startup_msg) {
        msg = startup_msg;
    } else {
        WaitPort(g->dosport);
        msg = GetMsg(g->dosport);
    }
    pkt = (struct DosPacket *)msg->mn_Node.ln_Name;

    g->devnode = (struct DeviceNode *)BADDR(pkt->dp_Arg3);
    fssm = (struct FileSysStartupMsg *)BADDR(pkt->dp_Arg2);
    g->fssm = fssm;

    /* set up logging before any startup error path can fire; with
     * logging compiled out, g is MEMF_CLEAR-allocated so g->log is
     * already zeroed and keeping log.o out of the link saves ROM space */
#if ODFS_FEATURE_LOG
    odfs_log_init(&g->log);
    odfs_log_set_sink(&g->log, log_sink, NULL);
    odfs_log_set_level(&g->log, ODFS_LOG_INFO);
#if ODFS_PACKET_TRACE
    odfs_log_set_level(&g->log, ODFS_LOG_TRACE);
#endif
#endif
    ODFS_INFO(&g->log, ODFS_SUB_NONE,
              "ODFileSystem " ODFS_GIT_VERSION
              " (" ODFS_AMIGA_DATE ") starting...");

    if (!odfs_amiga_open_libraries()) {
        ODFS_ERROR(&g->log, ODFS_SUB_CORE,
                   "open dos.library failed");
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = ERROR_INVALID_RESIDENT_LIBRARY;
        return_packet(g, pkt);
        odfs_amiga_free_mem(g, sizeof(*g));
        return;
    }
    g->dosbase = odfs_amiga_dosbase();

    /*
     * Validate the FileSysStartupMsg before trusting any of its fields.
     * A mount entry that uses Handler= instead of FileSystem= (or lacks
     * Device=) starts the handler without an FSSM: dp_Arg2 is then zero
     * or a BCPL string, and blindly reading through it yields a garbage
     * device name and flags (issue #7's "OpenDevice failed device=
     * unit=0 flags=16255938"). Fail with a message that names the fix.
     */
    de = fssm ? (struct DosEnvec *)BADDR(fssm->fssm_Environ) : NULL;
    if (!fssm || !de) {
        ODFS_ERROR(&g->log, ODFS_SUB_CORE,
                   "startup packet has no FileSysStartupMsg/environment; "
                   "mount entry must use FileSystem= (not Handler=) "
                   "with Device= and Unit=");
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = ERROR_REQUIRED_ARG_MISSING;
        return_packet(g, pkt);
        goto shutdown;
    }

    {
        int len = AROS_BSTR_strlen(fssm->fssm_Device);
        if (len >= (int)sizeof(g->devname))
            len = sizeof(g->devname) - 1;
        memcpy(g->devname, AROS_BSTR_ADDR(fssm->fssm_Device), len);
        g->devname[len] = '\0';
    }
    if (g->devname[0] == '\0') {
        ODFS_ERROR(&g->log, ODFS_SUB_CORE,
                   "startup message names no exec device; set Device= "
                   "in the mount entry (or the DOSDriver icon tooltype)");
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = ERROR_REQUIRED_ARG_MISSING;
        return_packet(g, pkt);
        goto shutdown;
    }
    g->devunit = fssm->fssm_Unit;
    g->devflags = fssm->fssm_Flags;

    g->envec = de;
    g->sector_size = de->de_SizeBlock << 2;
    if (g->sector_size == 0) {
        ODFS_WARN(&g->log, ODFS_SUB_CORE,
                  "mount entry sets no block size; assuming 2048");
        g->sector_size = 2048;
    }

    ODFS_INFO(&g->log, ODFS_SUB_CORE, "libraries open, device=%s unit=%lu",
              g->devname, (unsigned long)g->devunit);

    {
        char devnode_name[32];

        devnode_name[0] = '\0';
        if (device_node_name_conflicts(g, devnode_name, sizeof(devnode_name))) {
            ODFS_ERROR(&g->log, ODFS_SUB_CORE,
                       "device node %s already exists; refusing duplicate",
                       devnode_name[0] ? devnode_name : "<unnamed>");
            pkt->dp_Res1 = DOSFALSE;
            pkt->dp_Res2 = ERROR_OBJECT_EXISTS;
            return_packet(g, pkt);
            goto shutdown;
        }
    }

    /* open device */
    g->devport = odfs_amiga_create_msg_port();
    if (!g->devport) {
        ODFS_ERROR(&g->log, ODFS_SUB_IO,
                   "CreateMsgPort failed for %s unit=%lu",
                   g->devname, (unsigned long)g->devunit);
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = ERROR_NO_FREE_STORE;
        return_packet(g, pkt);
        goto shutdown;
    }

    g->devreq = (struct IOStdReq *)odfs_amiga_create_io_request(
        g->devport, sizeof(struct IOStdReq));
    if (!g->devreq) {
        ODFS_ERROR(&g->log, ODFS_SUB_IO,
                   "CreateIORequest failed for %s unit=%lu",
                   g->devname, (unsigned long)g->devunit);
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = ERROR_NO_FREE_STORE;
        return_packet(g, pkt);
        goto shutdown;
    }

    ODFS_INFO(&g->log, ODFS_SUB_IO, "opening %s unit %lu",
              g->devname, (unsigned long)g->devunit);
    if (OpenDevice((CONST_STRPTR)g->devname, g->devunit,
                   (struct IORequest *)g->devreq, g->devflags) != 0) {
        ODFS_ERROR(&g->log, ODFS_SUB_IO,
                   "OpenDevice failed device=%s unit=%lu flags=%lu",
                   g->devname,
                   (unsigned long)g->devunit,
                   (unsigned long)g->devflags);
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = ERROR_DEVICE_NOT_MOUNTED;
        return_packet(g, pkt);
        goto shutdown;
    }
    ODFS_INFO(&g->log, ODFS_SUB_IO, "device open");

    g->devnode->dn_Startup = MKBADDR(fssm);
    g->devnode->dn_Task = g->dosport;

    /*
     * Allocate DMA-safe bounce buffer using de_BufMemType.
     * 16-byte aligned for 68040 DMA performance (CDVDFS pattern).
     * Size: 32 sectors (64KB) to cover common 32KB file reads in a
     * single device request while leaving room for larger callers.
     */
    {
        #define DMA_BUF_SECTORS  32
        ULONG memtype = de->de_BufMemType | MEMF_PUBLIC;
        ULONG raw_size = DMA_BUF_SECTORS * g->sector_size + 15;
        g->dma_buf_raw = (uint8_t *)odfs_amiga_alloc_mem(raw_size, memtype);
        if (!g->dma_buf_raw) {
            /* fallback: try without specific memory type */
            g->dma_buf_raw = (uint8_t *)odfs_amiga_alloc_mem(raw_size,
                                                             MEMF_PUBLIC);
        }
        if (g->dma_buf_raw) {
            /* 16-byte align */
            g->dma_buf = (uint8_t *)(((ULONG)g->dma_buf_raw + 15) & ~15UL);
            g->dma_buf_size = DMA_BUF_SECTORS * g->sector_size;
        } else {
            ODFS_ERROR(&g->log, ODFS_SUB_IO,
                       "AllocMem failed for DMA buffer size=%lu memtype=%lu",
                       (unsigned long)raw_size,
                       (unsigned long)memtype);
            pkt->dp_Res1 = DOSFALSE;
            pkt->dp_Res2 = ERROR_NO_FREE_STORE;
            return_packet(g, pkt);
            goto shutdown;
        }
    }

    /*
     * Probe the drive geometry before committing to the mount.
     * TD_GETGEOMETRY uses the native ATA path and returns promptly even
     * on a not-ready unit (unlike HD_SCSICMD, which can hang).
     *
     * TDERR_DiskChanged from a unit that also answers TD_CHANGESTATE
     * is a real drive with no medium in it. Keep the mount and wait
     * for a disc: declining would leave no handler running to see the
     * insertion (issue #8) — the drive only came back after a manual
     * access restarted the handler. Geometry (and the MODE SELECT
     * block size fixup) is re-probed when media arrives.
     *
     * Any other failure means the unit has no usable device behind
     * it — e.g. the empty/phantom second ATAPI channel that QEMU's
     * peg2ide reports from a floating bus. Decline the mount in that
     * case rather than publishing a dead drive that DOS would route
     * to and poll.
     */
    {
        LONG geo_rc = probe_drive_geometry(g);

        if (geo_rc == IOERR_NOCMD) {
            ODFS_INFO(&g->log, ODFS_SUB_IO,
                      "geometry unavailable; using mountlist sector=%lu",
                      (unsigned long)g->sector_size);
        } else if (geo_rc == TDERR_DiskChanged &&
                   query_media_present(g, NULL)) {
            ODFS_INFO(&g->log, ODFS_SUB_IO,
                      "no medium in unit %lu; waiting for a disc",
                      (unsigned long)g->devunit);
            g->geo_pending = 1;
        } else if (geo_rc != 0) {
            ODFS_WARN(&g->log, ODFS_SUB_IO,
                      "no usable device on unit %lu (geometry rc=%ld) - "
                      "declining mount",
                      (unsigned long)g->devunit, (long)geo_rc);
            pkt->dp_Res1 = DOSFALSE;
            pkt->dp_Res2 = ERROR_DEVICE_NOT_MOUNTED;
            return_packet(g, pkt);
            goto shutdown;
        }
    }
    ODFS_INFO(&g->log, ODFS_SUB_IO, "scsi setup done");

    /* set up media adapter (context lives in g, one per process) */
    g->media_ctx.g = g;
    g->media.ops = &amiga_media_ops;
    g->media.ctx = &g->media_ctx;

#if ODFS_AMIGA_OS4
    {
        LONG err_dos = activate_vector_port(g);
        if (err_dos != 0) {
            ODFS_ERROR(&g->log, ODFS_SUB_CORE,
                       "OS4 filesystem vector port setup failed: %ld",
                       (long)err_dos);
            pkt->dp_Res1 = DOSFALSE;
            pkt->dp_Res2 = err_dos;
            return_packet(g, pkt);
            goto shutdown;
        }
    }
#endif

    /* reply startup packet */
    pkt->dp_Res1 = DOSTRUE;
    pkt->dp_Res2 = 0;
    return_packet(g, pkt);

    /* publish nodes after replying so DOS has released the device list lock */
    publish_device_node(g);

    /*
     * Install the media change interrupt before the first mount
     * attempt: a disc inserted after mount_volume() has decided the
     * drive is empty then raises a signal the main loop picks up,
     * instead of going unseen until the next change event. A change
     * that slips in between install and mount at worst remounts a
     * freshly mounted disc, which holds no locks yet.
     */
    install_media_change(g);
    mount_volume(g);

    /* ---- main packet loop ---- */
    dossig = 1UL << g->dosport->mp_SigBit;
    chgsig = (g->chgsigbit >= 0) ? (1UL << g->chgsigbit) : 0;

    while (running) {
        pubsig = g->publish_timer_port ?
            (1UL << g->publish_timer_port->mp_SigBit) : 0;
        waitmask = dossig | chgsig | pubsig;
        sigs = Wait(waitmask);

        /* media change */
        if ((sigs & chgsig) && !g->inhibited) {
            ODFS_FS_LOCK(g);
            handle_media_change(g);
            ODFS_FS_UNLOCK(g);
            /* re-init media adapter after remount */
            g->media_ctx.g = g;
        }

        /* DOS packets */
        if (sigs & dossig) {
            while ((msg = GetMsg(g->dosport)) != NULL) {
                pkt = (struct DosPacket *)msg->mn_Node.ln_Name;
#if ODFS_SERIAL_DEBUG && ODFS_PACKET_TRACE
                trace_pkt(g, "dequeue", pkt);
#endif

#if ODFS_AMIGA_OS4
                /* hand-built packets and private messages reach this
                 * port directly; validate before trusting the packet */
                if (!pkt || pkt->dp_Link != msg) {
                    ReplyMsg(msg);
                    continue;
                }
                msg->mn_ReplyPort = pkt->dp_Port;
#endif

                if (pkt->dp_Type == ACTION_DIE ||
                    pkt->dp_Type == ACTION_SHUTDOWN) {
                    shutdown_pkt = pkt;
                    running = 0;
                    break;
                }

                if (!g->mounted && packet_mount_need(pkt) > 0) {
                    pkt->dp_Res1 = DOSFALSE;
                    pkt->dp_Res2 = ERROR_NO_DISK;
                    return_packet(g, pkt);
                    continue;
                }

                /*
                 * Service DOS packets with the shared packet dispatcher.
                 * On OS4 this dos.library drives the handler through the
                 * classic packet protocol (BPTR locks) for the legacy
                 * Lock()/Examine()/ExNext() APIs, including the
                 * deprecated examine actions that DOSEmulatePacket
                 * answers with ERROR_ACTION_NOT_KNOWN. Hold the
                 * filesystem semaphore so packet servicing in the
                 * handler process is serialized against native
                 * vector-port calls made from caller context.
                 */
                ODFS_FS_LOCK(g);
                handle_packet(g, pkt);
                ODFS_FS_UNLOCK(g);
                return_packet(g, pkt);

                ODFS_FS_LOCK(g);
                reap_stale_volumes(g);
                ODFS_FS_UNLOCK(g);
            }
        }

        if (sigs & pubsig) {
            ODFS_FS_LOCK(g);
            handle_volume_publish_retry(g);
            ODFS_FS_UNLOCK(g);
        }
    }

    /*
     * ---- shutdown ----
     * ACTION_SHUTDOWN (V51+, from DismountDevice()) passes DismountDevice
     * flags in dp_Arg1; DMDF_KEEPDEVICE asks us to leave the device node
     * published for a later remount. ACTION_DIE carries no flags, so it
     * keeps the historical behavior of removing the node. Only OS4 ever
     * sends ACTION_SHUTDOWN, so classic/ROM builds fold this to 0.
     */
#if ODFS_AMIGA_OS4
    keep_device = (shutdown_pkt &&
                   shutdown_pkt->dp_Type == ACTION_SHUTDOWN &&
                   (shutdown_pkt->dp_Arg1 & DMDF_KEEPDEVICE)) ? 1 : 0;
    if (keep_device)
        ODFS_INFO(&g->log, ODFS_SUB_CORE,
                  "shutdown: keeping device node for remount (DMDF_KEEPDEVICE)");
#else
    keep_device = 0;
#endif

    /*
     * Invalidate the vector port first so dos.library stops vectoring new
     * callers, then tear down DOS-visible state while holding the
     * filesystem semaphore so in-flight vector calls finish first. The
     * shutdown packet is replied only after the teardown is done.
     */
    ODFS_FS_LOCK(g);
#if ODFS_AMIGA_OS4
    odfs_os4_invalidate_vector_port(g->vector_port);
#endif
    remove_media_change(g);
    unmount_volume(g);
    drain_all_objects(g);
    unpublish_device_node(g, keep_device);
    ODFS_FS_UNLOCK(g);

    if (shutdown_pkt) {
        shutdown_pkt->dp_Res1 = DOSTRUE;
        shutdown_pkt->dp_Res2 = 0;
        return_packet(g, shutdown_pkt);
    }

shutdown:
    destroy_volume_publish_timer(g);
#if ODFS_AMIGA_OS4
    release_vector_io_request(g);
#endif
    if (g->devreq) {
        if (g->devreq->io_Device)
            CloseDevice((struct IORequest *)g->devreq);
        odfs_amiga_delete_io_request((struct IORequest *)g->devreq);
    }
    if (g->devport)
        odfs_amiga_delete_msg_port(g->devport);

    pool_drain(&g->entry_pool, sizeof(odfs_entry_t));
    pool_drain(&g->lock_pool, sizeof(odfs_lock_t));
    pool_drain(&g->fh_pool, sizeof(odfs_fh_t));

    /* free DMA bounce buffer */
    if (g->dma_buf_raw)
        odfs_amiga_free_mem(g->dma_buf_raw,
                            DMA_BUF_SECTORS * g->sector_size + 15);

    if (g->devnode && g->devnode->dn_Task == g->dosport)
        g->devnode->dn_Task = NULL;

#if ODFS_AMIGA_OS4
    deactivate_vector_port(g);
#endif

    odfs_amiga_close_libraries();
    odfs_amiga_free_mem(g, sizeof(*g));
}

void handler_main(void)
{
    handler_main_startup(NULL);
}
