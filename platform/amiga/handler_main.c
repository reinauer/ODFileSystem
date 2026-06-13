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
#include <exec/io.h>
#include <devices/trackdisk.h>
#include <devices/scsidisk.h>
#include <devices/input.h>
#include <devices/inputevent.h>
#include <dos/dostags.h>
#include <dos/exall.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include <string.h>

#include "odfs/error.h"
#include "odfs/ancestry.h"
#include "odfs/alloc.h"
#include "odfs/string.h"

#ifndef ODFS_GIT_VERSION
#define ODFS_GIT_VERSION "unknown"
#endif

static const char version_string[] __attribute__((used)) =
    "$VER: ODFileSystem " ODFS_GIT_VERSION
    " (" ODFS_AMIGA_DATE ")";

/* forward declarations */
static void handle_packet(handler_global_t *g, struct DosPacket *pkt);
static void return_packet(handler_global_t *g, struct DosPacket *pkt);
static void publish_device_node(handler_global_t *g);
static void unpublish_device_node(handler_global_t *g);
#if ODFS_AMIGA_OS4
static LONG activate_vector_port(handler_global_t *g);
static void deactivate_vector_port(handler_global_t *g);
#endif
static void mount_volume(handler_global_t *g);
static void unmount_volume(handler_global_t *g);
static void free_volume(odfs_volume_t *volume);
static void destroy_device_node(struct DeviceNode *devnode);
static void destroy_volume_node(struct DeviceList *volnode);
static void detach_volume_node(struct DeviceList *volnode);
static int node_is_mount_root(const handler_global_t *g, const odfs_node_t *fnode);
static int query_media_change_count(handler_global_t *g, ULONG *count);
static int query_media_present(handler_global_t *g, ULONG *status);
static void fill_volume_date(handler_global_t *g, struct DateStamp *stamp);
static void notify_workbench_disk_change(BOOL inserted);
#if ODFS_FEATURE_CDDA
static int toc_has_data_track(const odfs_toc_t *toc);
static void copy_pure_audio_volume_name(handler_global_t *g);
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

static LONG changeint_signal(APTR data)
{
    odfs_changeint_data_t *ci = data;

    if (ci && ci->task && ci->sigmask)
        Signal(ci->task, ci->sigmask);
    return 0;
}

static int odfs_timestamp_to_datestamp(const odfs_timestamp_t *ts,
                                       struct DateStamp *stamp)
{
    static const int mdays[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
    LONG days = 0;
    int y;
    int m;

    if (!ts || !stamp || ts->year < 1978 || ts->month < 1 || ts->month > 12 ||
        ts->day < 1 || ts->day > 31 || ts->hour > 23 || ts->minute > 59 ||
        ts->second > 59)
        return 0;

    for (y = 1978; y < ts->year; y++) {
        days += 365;
        if ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)
            days++;
    }

    for (m = 1; m < ts->month; m++) {
        days += mdays[m];
        if (m == 2 && ((ts->year % 4 == 0 && ts->year % 100 != 0) ||
            ts->year % 400 == 0))
            days++;
    }

    days += ts->day - 1;
    stamp->ds_Days = days;
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

static odfs_err_t amiga_read_sectors(void *ctx, uint32_t lba,
                                      uint32_t count, void *buf)
{
    amiga_media_ctx_t *am = ctx;
    handler_global_t *g = am->g;
    uint32_t total_bytes = count * g->sector_size;
    uint8_t *out = buf;
    uint32_t done = 0;

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

        g->devreq->io_Offset = byte_offset_lo;
        g->devreq->io_Actual = byte_offset_hi;
        g->devreq->io_Length = chunk;
        g->devreq->io_Data   = g->dma_buf;

        if (byte_offset_hi != 0)
            g->devreq->io_Command = TD_READ64;
        else
            g->devreq->io_Command = CMD_READ;

        if (DoIO((struct IORequest *)g->devreq) != 0 ||
            g->devreq->io_Error != 0 ||
            g->devreq->io_Actual != chunk) {
            ODFS_ERROR(&g->log, ODFS_SUB_IO,
                       "sector read failed lba=%lu count=%lu "
                       "chunk=%lu io_Error=%ld actual=%lu cmd=%lu",
                       (unsigned long)cur_lba,
                       (unsigned long)count,
                       (unsigned long)chunk,
                       (long)g->devreq->io_Error,
                       (unsigned long)g->devreq->io_Actual,
                       (unsigned long)g->devreq->io_Command);
            return ODFS_ERR_IO;
        }

        memcpy(out + done, g->dma_buf, chunk);
        done += chunk;
    }

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

static odfs_err_t amiga_read_last_session_lba(void *ctx, uint32_t *lba_out)
{
    amiga_media_ctx_t *am = ctx;
    handler_global_t *g = am->g;
    uint8_t cmd[10];
    uint8_t buf[12];
    uint8_t sense[32];
    struct SCSICmd scsi;
    LONG io_rc;

    if (!lba_out)
        return ODFS_ERR_INVAL;

    *lba_out = 0;

    if (g->last_session_passthrough == 0)
        return ODFS_ERR_UNSUPPORTED;

    memset(cmd, 0, sizeof(cmd));
    memset(buf, 0, sizeof(buf));
    memset(sense, 0, sizeof(sense));
    memset(&scsi, 0, sizeof(scsi));

    cmd[0] = 0x43;  /* READ TOC/PMA/ATIP */
    cmd[1] = 0x00;  /* MSF=0 (LBA) */
    cmd[2] = 0x01;  /* format: multisession info */
    cmd[7] = 0x00;
    cmd[8] = sizeof(buf);

    scsi.scsi_Data = (UWORD *)buf;
    scsi.scsi_Length = sizeof(buf);
    scsi.scsi_CmdLength = 10;
    scsi.scsi_Command = cmd;
    scsi.scsi_Flags = SCSIF_READ | SCSIF_AUTOSENSE;
    scsi.scsi_SenseData = sense;
    scsi.scsi_SenseLength = sizeof(sense);

    g->devreq->io_Command = HD_SCSICMD;
    g->devreq->io_Data    = &scsi;
    g->devreq->io_Length  = sizeof(scsi);

    io_rc = DoIO((struct IORequest *)g->devreq);
    if (io_rc != 0 || g->devreq->io_Error != 0 || scsi.scsi_Status != 0) {
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
                  (long)g->devreq->io_Error,
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
    LONG io_rc;

    memset(cmd, 0, sizeof(cmd));
    memset(sense, 0, sizeof(sense));
    memset(&scsi, 0, sizeof(scsi));

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

    scsi.scsi_Data      = (UWORD *)buf;
    scsi.scsi_Length     = count * 2352;
    scsi.scsi_CmdLength  = 12;
    scsi.scsi_Command    = cmd;
    scsi.scsi_Flags      = SCSIF_READ | SCSIF_AUTOSENSE;
    scsi.scsi_SenseData  = sense;
    scsi.scsi_SenseLength = sizeof(sense);

    g->devreq->io_Command = HD_SCSICMD;
    g->devreq->io_Data    = &scsi;
    g->devreq->io_Length  = sizeof(scsi);

    io_rc = DoIO((struct IORequest *)g->devreq);
    if (io_rc != 0 || g->devreq->io_Error != 0 || scsi.scsi_Status != 0 ||
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
                   (long)g->devreq->io_Error,
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
    LONG io_rc;

    memset(toc, 0, sizeof(*toc));
    memset(cmd, 0, sizeof(cmd));
    memset(buf, 0, sizeof(buf));
    memset(sense, 0, sizeof(sense));
    memset(&scsi, 0, sizeof(scsi));

    if (g->toc_passthrough == 0)
        return ODFS_ERR_UNSUPPORTED;

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
    scsi.scsi_SenseData = sense;
    scsi.scsi_SenseLength = sizeof(sense);

    g->devreq->io_Command = HD_SCSICMD;
    g->devreq->io_Data    = &scsi;
    g->devreq->io_Length  = sizeof(scsi);

    io_rc = DoIO((struct IORequest *)g->devreq);
    if (io_rc != 0 || g->devreq->io_Error != 0 || scsi.scsi_Status != 0) {
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
                  (long)g->devreq->io_Error,
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
    memset(&scsi, 0, sizeof(scsi));

    cmd[0] = 0x43;  /* READ TOC/PMA/ATIP */
    cmd[1] = 0x00;  /* MSF=0 */
    cmd[2] = 0x05;  /* format: CD-Text */
    cmd[7] = 0x00;
    cmd[8] = sizeof(hdr);

    scsi.scsi_Data = (UWORD *)hdr;
    scsi.scsi_Length = sizeof(hdr);
    scsi.scsi_CmdLength = 10;
    scsi.scsi_Command = cmd;
    scsi.scsi_Flags = SCSIF_READ | SCSIF_AUTOSENSE;
    scsi.scsi_SenseData = sense;
    scsi.scsi_SenseLength = sizeof(sense);

    g->devreq->io_Command = HD_SCSICMD;
    g->devreq->io_Data    = &scsi;
    g->devreq->io_Length  = sizeof(scsi);

    io_rc = DoIO((struct IORequest *)g->devreq);
    if (io_rc != 0 || g->devreq->io_Error != 0 || scsi.scsi_Status != 0) {
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
                  (long)g->devreq->io_Error,
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
    memset(&scsi, 0, sizeof(scsi));

    cmd[0] = 0x43;  /* READ TOC/PMA/ATIP */
    cmd[1] = 0x00;  /* MSF=0 */
    cmd[2] = 0x05;  /* format: CD-Text */
    cmd[7] = (uint8_t)(total_len >> 8);
    cmd[8] = (uint8_t)(total_len);

    scsi.scsi_Data = (UWORD *)buf;
    scsi.scsi_Length = (ULONG)total_len;
    scsi.scsi_CmdLength = 10;
    scsi.scsi_Command = cmd;
    scsi.scsi_Flags = SCSIF_READ | SCSIF_AUTOSENSE;
    scsi.scsi_SenseData = sense;
    scsi.scsi_SenseLength = sizeof(sense);

    g->devreq->io_Command = HD_SCSICMD;
    g->devreq->io_Data    = &scsi;
    g->devreq->io_Length  = sizeof(scsi);

    io_rc = DoIO((struct IORequest *)g->devreq);
    if (io_rc != 0 || g->devreq->io_Error != 0 || scsi.scsi_Status != 0 ||
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
                  (long)g->devreq->io_Error,
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

    g->devreq->io_Command = HD_SCSICMD;
    g->devreq->io_Data    = &scsi;
    g->devreq->io_Length  = sizeof(scsi);

    io_rc = DoIO((struct IORequest *)g->devreq);
    if (io_rc != 0 || g->devreq->io_Error != 0 || scsi.scsi_Status != 0) {
        ODFS_WARN(&g->log, ODFS_SUB_IO,
                  "MODE SELECT failed block_length=%lu io_rc=%ld "
                  "io_Error=%ld scsi_Status=%lu",
                  (unsigned long)block_length,
                  (long)io_rc,
                  (long)g->devreq->io_Error,
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
    .read_audio            = amiga_read_audio,
    .read_cdtext           = amiga_read_cdtext,
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

static struct DeviceList *volume_node_ptr(const odfs_volume_t *volume)
{
    return volume ? volume->volnode : NULL;
}

static odfs_entry_t *alloc_entry(odfs_volume_t *volume,
                                 const odfs_node_t *fnode,
                                 const odfs_node_t *parent)
{
    odfs_entry_t *entry;

    entry = odfs_amiga_alloc_mem(sizeof(*entry), MEMF_PUBLIC | MEMF_CLEAR);
    if (!entry)
        return NULL;

    entry->volume = volume;
    entry->fnode = *fnode;
    if (parent)
        entry->parent_node = *parent;
    else
        entry->parent_node = *fnode;
    entry->refcount = 1;
    return entry;
}

static odfs_entry_t *retain_entry(odfs_entry_t *entry)
{
    if (entry)
        entry->refcount++;
    return entry;
}

static void release_entry(odfs_entry_t *entry)
{
    if (!entry)
        return;
    if (--entry->refcount == 0)
        odfs_amiga_free_mem(entry, sizeof(*entry));
}

static odfs_node_t *lock_node(odfs_lock_t *ol)
{
    return ol ? &ol->entry->fnode : NULL;
}

static odfs_node_t *lock_parent_node(odfs_lock_t *ol)
{
    return ol ? &ol->entry->parent_node : NULL;
}

static odfs_node_t *fh_node(odfs_fh_t *fh)
{
    return fh ? &fh->entry->fnode : NULL;
}

static odfs_node_t *fh_parent_node(odfs_fh_t *fh)
{
    return fh ? &fh->entry->parent_node : NULL;
}

static odfs_volume_t *fh_volume(odfs_fh_t *fh)
{
    return fh ? fh->entry->volume : NULL;
}

static int lock_is_active(handler_global_t *g, odfs_lock_t *needle)
{
    odfs_lock_t *ol;

    if (!g || !needle)
        return 0;

    for (ol = (odfs_lock_t *)g->locklist.mlh_Head;
         ol->node.mln_Succ != NULL;
         ol = (odfs_lock_t *)ol->node.mln_Succ) {
        if (ol == needle)
            return 1;
    }
    return 0;
}

static int fh_is_active(handler_global_t *g, odfs_fh_t *needle)
{
    odfs_fh_t *fh;

    if (!g || !needle)
        return 0;

    for (fh = (odfs_fh_t *)g->fhlist.mlh_Head;
         fh->node.mln_Succ != NULL;
         fh = (odfs_fh_t *)fh->node.mln_Succ) {
        if (fh == needle)
            return 1;
    }
    return 0;
}

static odfs_volume_t *alloc_volume(handler_global_t *g, struct DeviceList *volnode)
{
    odfs_volume_t *volume;

    volume = odfs_amiga_alloc_mem(sizeof(*volume), MEMF_PUBLIC | MEMF_CLEAR);
    if (!volume)
        return NULL;

    volume->volnode = volnode;
    volume->id = g->next_volume_id++;
    return volume;
}

static void rebuild_volume_locklist(handler_global_t *g, odfs_volume_t *volume)
{
    odfs_lock_t *ol;
    odfs_lock_t *prev = NULL;
    BPTR head = 0;

    if (!volume || !volume->volnode)
        return;

    Forbid();
    for (ol = (odfs_lock_t *)g->locklist.mlh_Head;
         ol->node.mln_Succ != NULL;
         ol = (odfs_lock_t *)ol->node.mln_Succ) {
        if (ol->entry->volume != volume)
            continue;

        if (!head)
            head = LOCK_TO_BPTR(ol);
        if (prev)
            ODFS_LOCK_DOS(prev)->fl_Link = LOCK_TO_BPTR(ol);
        prev = ol;
    }

    if (prev)
        ODFS_LOCK_DOS(prev)->fl_Link = 0;
    volume->volnode->dl_LockList = head;
    Permit();
}

static void destroy_stale_volume(handler_global_t *g, odfs_volume_t *volume)
{
    if (!volume)
        return;

    if (volume->volnode) {
        detach_volume_node(volume->volnode);
        destroy_volume_node(volume->volnode);
    }
    if (g->volnode == volume->volnode)
        g->volnode = NULL;
    free_volume(volume);
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

    rebuild_volume_locklist(g, volume);
    if (volume->object_count == 0)
        destroy_stale_volume(g, volume);
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

ULONG odfs_handler_node_key(const odfs_node_t *node)
{
    return amiga_node_key(node);
}

ULONG odfs_handler_node_protection(const odfs_node_t *node)
{
    ULONG prot = 0;

    if (!node)
        return 0;

    if (node->amiga_as.has_protection) {
        prot = node->amiga_as.protection[3];
    } else if (node->mode != 0) {
        /* MakeCD table 6 default mapping from PX to classic Amiga bits. */
        if ((node->mode & 0200) == 0)
            prot |= FIBF_DELETE | FIBF_WRITE;
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
        prot = FIBF_WRITE | FIBF_DELETE;
    }

    return prot;
}

void odfs_handler_node_date(const odfs_node_t *node, struct DateStamp *ds)
{
    if (!ds)
        return;

    memset(ds, 0, sizeof(*ds));
    if (!node || node->mtime.year < 1978)
        return;

    {
        LONG days = 0;
        int y;

        for (y = 1978; y < node->mtime.year; y++) {
            days += 365;
            if ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)
                days++;
        }
        {
            static const int mdays[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
            int m;
            for (m = 1; m < node->mtime.month && m <= 12; m++) {
                days += mdays[m];
                if (m == 2 && ((node->mtime.year % 4 == 0 &&
                    node->mtime.year % 100 != 0) ||
                    node->mtime.year % 400 == 0))
                    days++;
            }
        }
        days += node->mtime.day - 1;

        ds->ds_Days   = days;
        ds->ds_Minute = node->mtime.hour * 60 + node->mtime.minute;
        ds->ds_Tick   = node->mtime.second * TICKS_PER_SECOND;
    }
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
    if (volume)
        odfs_amiga_free_mem(volume, sizeof(*volume));
}

static void mount_volume(handler_global_t *g);
static void unmount_volume(handler_global_t *g);

static void drain_all_objects(handler_global_t *g)
{
    struct Node *node;

    while ((node = RemHead((struct List *)&g->fhlist)) != NULL) {
        odfs_fh_t *fh = (odfs_fh_t *)node;
        release_volume_object(g, fh->entry->volume);
        release_entry(fh->entry);
        odfs_amiga_free_mem(fh, sizeof(*fh));
    }

    while ((node = RemHead((struct List *)&g->locklist)) != NULL) {
        odfs_lock_t *ol = (odfs_lock_t *)node;
#if ODFS_AMIGA_OS4
        if (ol->lock)
            FreeDosObject(DOS_LOCK, ol->lock);
#endif
        release_volume_object(g, ol->entry->volume);
        release_entry(ol->entry);
        odfs_amiga_free_mem(ol, sizeof(*ol));
    }
}

static int packet_needs_live_mount(const struct DosPacket *pkt)
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
        return 0;
    default:
        return 1;
    }
}

static odfs_lock_t *alloc_lock(handler_global_t *g,
                                const odfs_node_t *fnode,
                                const odfs_node_t *parent,
                                LONG access)
{
    odfs_lock_t *ol;
    odfs_entry_t *entry;
    struct FileLock *lock;

    if (!g->current_volume)
        return NULL;

    entry = alloc_entry(g->current_volume, fnode, parent);
    if (!entry)
        return NULL;

    ol = odfs_amiga_alloc_mem(sizeof(*ol), MEMF_PUBLIC | MEMF_CLEAR);
    if (!ol) {
        release_entry(entry);
        return NULL;
    }
#if ODFS_AMIGA_OS4
    ol->lock = AllocDosObjectTags(DOS_LOCK,
                                  ADO_DOSType, ODFS_OS4_CD_DOSTYPE,
                                  TAG_DONE);
    if (!ol->lock) {
        odfs_amiga_free_mem(ol, sizeof(*ol));
        release_entry(entry);
        return NULL;
    }
#endif
    ol->entry = entry;
    ol->key = amiga_node_key(fnode);
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
    rebuild_volume_locklist(g, entry->volume);
    return ol;
}

static void free_lock(handler_global_t *g, odfs_lock_t *ol)
{
    if (!ol)
        return;
    Remove((struct Node *)&ol->node);
    rebuild_volume_locklist(g, ol->entry->volume);
#if ODFS_AMIGA_OS4
    if (ol->lock)
        FreeDosObject(DOS_LOCK, ol->lock);
#endif
    release_volume_object(g, ol->entry->volume);
    release_entry(ol->entry);
    odfs_amiga_free_mem(ol, sizeof(*ol));
}

static odfs_lock_t *dup_lock(handler_global_t *g, odfs_lock_t *src)
{
    odfs_lock_t *ol;
    struct FileLock *lock;

    if (!src)
        return NULL;

    ol = odfs_amiga_alloc_mem(sizeof(*ol), MEMF_PUBLIC | MEMF_CLEAR);
    if (!ol)
        return NULL;

#if ODFS_AMIGA_OS4
    ol->lock = AllocDosObjectTags(DOS_LOCK,
                                  ADO_DOSType, ODFS_OS4_CD_DOSTYPE,
                                  TAG_DONE);
    if (!ol->lock) {
        odfs_amiga_free_mem(ol, sizeof(*ol));
        return NULL;
    }
#endif
    ol->entry = retain_entry(src->entry);
    ol->key = src->key;
#if !ODFS_AMIGA_OS4
    ol->dos_private[0] = 0;
    ol->dos_private[1] = 0;
#endif
    lock = ODFS_LOCK_DOS(ol);
    lock->fl_Link = 0;
    lock->fl_Key = ol->key;
    lock->fl_Access = ODFS_LOCK_DOS(src)->fl_Access;
    lock->fl_Task = g->dosport;
    lock->fl_Volume = MKBADDR(volume_node_ptr(ol->entry->volume));
#if ODFS_AMIGA_OS4
    lock->fl_FSPrivate1 = ol;
    lock->fl_FSPrivate2 = ol->entry;
#endif
    retain_volume_object(ol->entry->volume);
    AddTail((struct List *)&g->locklist, (struct Node *)&ol->node);
    rebuild_volume_locklist(g, ol->entry->volume);
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

    fh = odfs_amiga_alloc_mem(sizeof(*fh), MEMF_PUBLIC | MEMF_CLEAR);
    if (!fh)
        return NULL;

    fh->entry = retain_entry(entry);
    fh->access = access;
    fh->pos = 0;
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
    release_entry(fh->entry);
    odfs_amiga_free_mem(fh, sizeof(*fh));
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
 * Tracks the current node and its immediate parent. When an ascent needs the
 * next ancestor, reconstruct it with an iterative directory walk.
 */
static odfs_err_t resolve_amiga_path(handler_global_t *g,
                                      const odfs_node_t *start,
                                      const odfs_node_t *start_parent,
                                      const char *path,
                                      odfs_node_t *result,
                                      odfs_node_t *parent_out)
{
    odfs_node_t cur = *start;
    odfs_node_t parent = start_parent ? *start_parent : *start;
    const char *p = path;
    char comp[256];
    odfs_err_t err;

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
            if (!node_is_mount_root(g, &cur)) {
                cur = parent;
                if (node_is_mount_root(g, &cur)) {
                    parent = g->mount.root;
#if ODFS_FEATURE_CDDA
                } else if (g->has_cdda && nodes_same(&cur, &g->cdda_root)) {
                    parent = g->mount.root;
#endif
                } else {
                    err = odfs_resolve_parent_node(&g->mount, &cur,
                                                   &parent, NULL);
                    if (err != ODFS_OK)
                        return err;
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
        if (cur.kind != ODFS_NODE_DIR)
            return ODFS_ERR_NOT_DIR;

#if ODFS_FEATURE_CDDA
        /* intercept "CDDA" virtual directory on mixed-mode discs */
        if (g->has_cdda && cur.extent.lba == g->mount.root.extent.lba &&
            odfs_strcasecmp(comp, "CDDA") == 0) {
            parent = cur;
            cur = g->cdda_root;
            p = end;
            continue;
        }
#endif

        parent = cur;
        err = lookup_child_node(g, &cur, comp, &cur);
        if (err != ODFS_OK)
            return err;

        p = end;
        if (*p == '/')
            p++;
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
    ULONG prot = 0;
    int comment_len = 0;

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
    odfs_amiga_set_fib_entry_type(fib, fib->fib_DirEntryType);
    fib->fib_Size         = (LONG)fnode->size;
    fib->fib_NumBlocks    = (fnode->size + 511) / 512;

    if (fnode->amiga_as.has_protection) {
        prot = fnode->amiga_as.protection[3];
    } else if (fnode->mode != 0) {
        /* MakeCD table 6 default mapping from PX to classic Amiga bits. */
        if ((fnode->mode & 0200) == 0)
            prot |= FIBF_DELETE | FIBF_WRITE;
        if ((fnode->mode & 0100) == 0)
            prot |= FIBF_EXECUTE;
        if ((fnode->mode & 0400) == 0)
            prot |= FIBF_READ;
#ifdef FIBF_GRP_DELETE
        if (fnode->mode & 0020)
            prot |= FIBF_GRP_DELETE;
#endif
#ifdef FIBF_GRP_EXECUTE
        if (fnode->mode & 0010)
            prot |= FIBF_GRP_EXECUTE;
#endif
#ifdef FIBF_GRP_WRITE
        if (fnode->mode & 0020)
            prot |= FIBF_GRP_WRITE;
#endif
#ifdef FIBF_GRP_READ
        if (fnode->mode & 0040)
            prot |= FIBF_GRP_READ;
#endif
#ifdef FIBF_OTR_DELETE
        if (fnode->mode & 0002)
            prot |= FIBF_OTR_DELETE;
#endif
#ifdef FIBF_OTR_EXECUTE
        if (fnode->mode & 0001)
            prot |= FIBF_OTR_EXECUTE;
#endif
#ifdef FIBF_OTR_WRITE
        if (fnode->mode & 0002)
            prot |= FIBF_OTR_WRITE;
#endif
#ifdef FIBF_OTR_READ
        if (fnode->mode & 0004)
            prot |= FIBF_OTR_READ;
#endif
    } else {
        /* Read-only fallback when there is no RR/AS metadata at all. */
        prot = FIBF_WRITE | FIBF_DELETE;
    }
    fib->fib_Protection = prot;

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

    if (fnode->amiga_as.has_comment) {
        comment_len = strlen(fnode->amiga_as.comment);
        if (comment_len > (int)sizeof(fib->fib_Comment) - 1)
            comment_len = (int)sizeof(fib->fib_Comment) - 1;
        fib->fib_Comment[0] = comment_len;
        if (comment_len > 0)
            memcpy(&fib->fib_Comment[1], fnode->amiga_as.comment, comment_len);
    }

    fib->fib_DiskKey = (LONG)amiga_node_key(fnode);
}

static int node_is_mount_root(const handler_global_t *g, const odfs_node_t *fnode)
{
    if (!g || !fnode)
        return 0;

    return odfs_node_matches_identity(fnode, &g->mount.root);
}

static void fill_root_fib(handler_global_t *g, struct FileInfoBlock *fib,
                          const odfs_node_t *fnode)
{
    int len;

    fill_fib(fib, fnode);

    fib->fib_DirEntryType = ST_ROOT;
    odfs_amiga_set_fib_entry_type(fib, ST_ROOT);

    len = strlen(g->volname);
    if (len > 30)
        len = 30;
    fib->fib_FileName[0] = len;
    memcpy(&fib->fib_FileName[1], g->volname, len);
}

/* ------------------------------------------------------------------ */
/* shared frontend operations                                          */
/* ------------------------------------------------------------------ */

LONG odfs_handler_lock_object(handler_global_t *g,
                              odfs_lock_t *parent_lock,
                              const char *path,
                              LONG access,
                              odfs_lock_t **out)
{
    odfs_node_t result, parent_node;
    odfs_err_t err;
    const odfs_node_t *start;
    const odfs_node_t *start_parent;
    odfs_lock_t *ol;

    if (out)
        *out = NULL;
    if (!g || !path || !out)
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
    } else {
        if (!g->mounted)
            return ERROR_NO_DISK;
        start = &g->mount.root;
        start_parent = &g->mount.root;
    }

    err = resolve_amiga_path(g, start, start_parent, path, &result,
                             &parent_node);
    if (err != ODFS_OK)
        return odfs_err_to_dos(err);

    if (result.kind == ODFS_NODE_DIR)
        access = SHARED_LOCK;

    ol = alloc_lock(g, &result, &parent_node, access);
    if (!ol)
        return ERROR_NO_FREE_STORE;

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
        ol = alloc_lock(g, &g->mount.root, &g->mount.root, SHARED_LOCK);
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
        ol = alloc_lock(g, &g->mount.root, &g->mount.root, SHARED_LOCK);
    } else {
        LONG err_dos;

        if (!fh_is_active(g, fh))
            return ERROR_OBJECT_NOT_FOUND;

        err_dos = validate_object_volume(g, fh_volume(fh));
        if (err_dos != 0)
            return err_dos;
        ol = alloc_lock(g, fh_node(fh), fh_parent_node(fh), SHARED_LOCK);
    }

    if (!ol)
        return ERROR_NO_FREE_STORE;

    *out = ol;
    return 0;
}

LONG odfs_handler_parent_lock_object(handler_global_t *g,
                                     odfs_lock_t *ol,
                                     odfs_lock_t **out)
{
    const odfs_node_t *parent_node;
    odfs_node_t new_parent;
    odfs_err_t err;
    odfs_lock_t *parent;

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

    if (node_is_mount_root(g, lock_node(ol)))
        return 0;

    parent_node = lock_parent_node(ol);
    if (node_is_mount_root(g, parent_node)) {
        new_parent = g->mount.root;
#if ODFS_FEATURE_CDDA
    } else if (g->has_cdda && nodes_same(parent_node, &g->cdda_root)) {
        new_parent = g->mount.root;
#endif
    } else {
        err = odfs_resolve_parent_node(&g->mount, parent_node, &new_parent,
                                       NULL);
        if (err != ODFS_OK)
            return odfs_err_to_dos(err);
    }

    parent = alloc_lock(g, parent_node, &new_parent, SHARED_LOCK);
    if (!parent)
        return ERROR_NO_FREE_STORE;

    *out = parent;
    return 0;
}

LONG odfs_handler_parent_fh_object(handler_global_t *g,
                                   odfs_fh_t *fh,
                                   odfs_lock_t **out)
{
    const odfs_node_t *parent_node;
    odfs_node_t new_parent;
    odfs_err_t err;
    odfs_lock_t *parent;

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

    if (node_is_mount_root(g, fh_node(fh)))
        return 0;

    parent_node = fh_parent_node(fh);
    if (node_is_mount_root(g, parent_node)) {
        new_parent = g->mount.root;
#if ODFS_FEATURE_CDDA
    } else if (g->has_cdda && nodes_same(parent_node, &g->cdda_root)) {
        new_parent = g->mount.root;
#endif
    } else {
        err = odfs_resolve_parent_node(&g->mount, parent_node, &new_parent,
                                       NULL);
        if (err != ODFS_OK)
            return odfs_err_to_dos(err);
    }

    parent = alloc_lock(g, parent_node, &new_parent, SHARED_LOCK);
    if (!parent)
        return ERROR_NO_FREE_STORE;

    *out = parent;
    return 0;
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
    odfs_node_t result, parent_node;
    odfs_err_t err;
    const odfs_node_t *start;
    const odfs_node_t *start_parent;
    odfs_entry_t *entry;
    odfs_fh_t *fh;

    if (out)
        *out = NULL;
    if (!g || !path || !out)
        return ERROR_REQUIRED_ARG_MISSING;

    if (mode != MODE_OLDFILE)
        return ERROR_DISK_WRITE_PROTECTED;

    if (dirlock) {
        LONG err_dos;

        if (!lock_is_active(g, dirlock))
            return ERROR_INVALID_LOCK;

        err_dos = validate_object_volume(g, dirlock->entry->volume);
        if (err_dos != 0)
            return err_dos;
        start = lock_node(dirlock);
        start_parent = lock_parent_node(dirlock);
    } else {
        if (!g->mounted)
            return ERROR_NO_DISK;
        start = &g->mount.root;
        start_parent = &g->mount.root;
    }

    err = resolve_amiga_path(g, start, start_parent, path, &result,
                             &parent_node);
    if (err != ODFS_OK)
        return odfs_err_to_dos(err);

    if (result.kind == ODFS_NODE_DIR)
        return ERROR_OBJECT_WRONG_TYPE;

    entry = alloc_entry(g->current_volume, &result, &parent_node);
    if (!entry)
        return ERROR_NO_FREE_STORE;

    fh = alloc_fh(g, entry, SHARED_LOCK);
    release_entry(entry);
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
    err = read_file_node(g, fh_node(fh), fh->pos, buf, &actual);
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

    fill_fib(ec->fib, entry);
    ec->found = 1;
    return ODFS_ERR_EOF; /* stop after one entry */
}

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
                                 odfs_node_t *entry_out,
                                 ULONG *key_out)
{
    const odfs_node_t *dir;
    ULONG dir_key;
    dir_next_ctx_t dc;
    uint32_t resume = 0;

    if (!g || !entry_out || !key_out)
        return ERROR_REQUIRED_ARG_MISSING;

    dir = ol ? lock_node(ol) : &g->mount.root;

    if (ol) {
        LONG err_dos = validate_object_volume(g, ol->entry->volume);
        if (err_dos != 0)
            return err_dos;
    } else if (!g->mounted) {
        return ERROR_NO_DISK;
    }

    if (dir->kind != ODFS_NODE_DIR)
        return ERROR_OBJECT_WRONG_TYPE;

    dir_key = ol ? ol->key : amiga_node_key(dir);
    memset(&dc, 0, sizeof(dc));
    dc.previous_key = previous_key;
    dc.first = (previous_key == 0 || previous_key == dir_key);

#if ODFS_FEATURE_CDDA
    if (g->has_cdda && !dc.first &&
        previous_key == amiga_node_key(&g->cdda_root))
        return ERROR_NO_MORE_ENTRIES;

    if (g->has_cdda && dir->backend == ODFS_BACKEND_CDDA) {
        (void)cdda_backend_ops.readdir(g->cdda_ctx, &g->mount.cache,
                                       &g->log, dir, dir_next_cb, &dc,
                                       &resume);
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

    if (node_is_mount_root(g, fnode))
        fill_root_fib(g, fib, fnode);
    else
        fill_fib(fib, fnode);
#if !ODFS_AMIGA_OS4
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
    ec.fib = fib;
    ec.previous_key = (ULONG)fib->fib_DiskKey;
    ec.first = (ec.previous_key == dir_key);
    ec.seen_previous = 0;
    ec.found = 0;

    /*
     * Match the Amiga CD filesystem model: Examine() leaves fib_DiskKey as
     * the directory key, and ExNext() returns each child's object key.  This
     * costs a rescan but avoids exposing private iterator offsets to
     * Workbench/icon.library.
     */

    /* check if CDDA virtual dir was already emitted */
#if ODFS_FEATURE_CDDA
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
            fill_fib(fib, &g->cdda_root);
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

static int exall_fill_entry(struct ExAllData **cursor, LONG *remaining,
                            LONG data, const odfs_node_t *entry)
{
    struct ExAllData *ed = *cursor;
    struct FileInfoBlock fib;
    const char *name = entry->name;
    const char *comment = "";
    size_t name_len = strlen(name) + 1u;
    size_t comment_len = 1u;
    size_t need;
    UBYTE *p;

    if (entry->amiga_as.has_comment) {
        comment = entry->amiga_as.comment;
        comment_len = strlen(comment) + 1u;
    }

    need = exall_fixed_size(data) + name_len;
    if (data >= ED_COMMENT)
        need += comment_len;
    need = exall_align_size(need);

    if (need > (size_t)*remaining)
        return 0;

    fill_fib(&fib, entry);
    memset(ed, 0, exall_fixed_size(data));

    p = ((UBYTE *)ed) + exall_fixed_size(data);
    if (data >= ED_COMMENT) {
        ed->ed_Comment = (STRPTR)p;
        memcpy(p, comment, comment_len);
        p += comment_len;
    }

    ed->ed_Name = (STRPTR)p;
    memcpy(p, name, name_len);

    if (data >= ED_TYPE)
        ed->ed_Type = fib.fib_DirEntryType;
    if (data >= ED_SIZE)
        ed->ed_Size = (ULONG)fib.fib_Size;
    if (data >= ED_PROTECTION)
        ed->ed_Prot = (ULONG)fib.fib_Protection;
    if (data >= ED_DATE) {
        ed->ed_Days = (ULONG)fib.fib_Date.ds_Days;
        ed->ed_Mins = (ULONG)fib.fib_Date.ds_Minute;
        ed->ed_Ticks = (ULONG)fib.fib_Date.ds_Tick;
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
    if (!exall_fill_entry(&ec->cursor, &ec->remaining, ec->data, entry)) {
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

    fill_fib(fib, fh_node(fh));
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

    odfs_amiga_copy_device_lock(devnode, g->devnode);
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
    struct DeviceNode *name_match = NULL;
    struct DeviceNode *shadow;
    char want[32];
    char have[32];

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

        if (!name_match) {
            device_node_name_from_bstr(iter->dn_Name, have, sizeof(have));
            if (ascii_strieq(have, want))
                name_match = iter;
        }
    }

    if (!g->published_devnode && name_match)
        g->published_devnode = name_match;

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

static void unpublish_device_node(handler_global_t *g)
{
    struct DeviceNode *devnode;

    devnode = g->published_devnode;
    if (!devnode)
        return;

    devnode->dn_Task = NULL;

    if (g->published_devnode_owned) {
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

static void detach_volume_node(struct DeviceList *volnode)
{
    if (!volnode || !volnode->dl_Task)
        return;

    if (AttemptLockDosList(LDF_VOLUMES | LDF_WRITE)) {
        RemDosEntry((struct DosList *)volnode);
        UnLockDosList(LDF_VOLUMES | LDF_WRITE);
    }
    volnode->dl_Task = NULL;
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
                  "FB=FILEBUFFERS/K/N",
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
#endif

static void mount_volume(handler_global_t *g)
{
    odfs_mount_opts_t opts;
    odfs_err_t err;

    if (g->mounted || g->inhibited)
        return;

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
            g->has_cdda = 1;
            g->mounted = 1;
            g->mount.root = g->cdda_root;
            g->mount.backend_ops = &cdda_backend_ops;
            g->mount.backend_ctx = g->cdda_ctx;
            g->mount.active_backend = ODFS_BACKEND_CDDA;
            odfs_mount_register_backend(&g->mount, ODFS_BACKEND_CDDA,
                                        &cdda_backend_ops, g->cdda_ctx,
                                        &g->cdda_root);
            copy_pure_audio_volume_name(g);
            ODFS_INFO(&g->log, ODFS_SUB_MOUNT,
                      "mounted pure audio CD via CDDA backend");
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
                g->has_cdda = 1;
                g->mounted = 1;
                g->mount.root = g->cdda_root;
                g->mount.backend_ops = &cdda_backend_ops;
                g->mount.backend_ctx = g->cdda_ctx;
                g->mount.active_backend = ODFS_BACKEND_CDDA;
                odfs_mount_register_backend(&g->mount, ODFS_BACKEND_CDDA,
                                            &cdda_backend_ops, g->cdda_ctx,
                                            &g->cdda_root);
                copy_pure_audio_volume_name(g);
                ODFS_INFO(&g->log, ODFS_SUB_MOUNT,
                          "mounted pure audio CD via CDDA backend");
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
        if (AttemptLockDosList(LDF_VOLUMES | LDF_WRITE)) {
            AddDosEntry((struct DosList *)g->volnode);
            UnLockDosList(LDF_VOLUMES | LDF_WRITE);
        }
        notify_workbench_disk_change(TRUE);
    }

}

static void unmount_volume(handler_global_t *g)
{
    odfs_volume_t *volume;

    if (!g->mounted)
        return;

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

    rebuild_volume_locklist(g, volume);
    detach_volume_node(volume->volnode);
    notify_workbench_disk_change(FALSE);
    if (volume->object_count != 0) {
        return;
    }

    destroy_stale_volume(g, volume);
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
    if (!g || !g->devreq)
        return 0;

    g->devreq->io_Command = TD_CHANGENUM;
    if (DoIO((struct IORequest *)g->devreq) != 0)
        return 0;

    if (count)
        *count = g->devreq->io_Actual;

    return 1;
}

static int query_media_present(handler_global_t *g, ULONG *status)
{
    ULONG actual;

    if (!g || !g->devreq)
        return 0;

    g->devreq->io_Command = TD_CHANGESTATE;
    if (DoIO((struct IORequest *)g->devreq) != 0)
        return 0;

    actual = g->devreq->io_Actual;
    if (status)
        *status = actual;

    return 1;
}

static void handle_media_change(handler_global_t *g)
{
    ULONG change_count;
    ULONG status;

    /*
     * Some drives/controllers deliver an initial or redundant change
     * interrupt after TD_ADDCHANGEINT. Ignore those unless the device's
     * change counter actually moved, otherwise we invalidate every
     * existing lock/filehandle by remounting the same disc.
     */
    if (query_media_change_count(g, &change_count)) {
        if (g->change_count_valid && change_count == g->change_count)
            return;
        g->change_count = change_count;
        g->change_count_valid = 1;
    }

    if (!query_media_present(g, &status))
        return;

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

void handler_main_startup(struct Message *startup_msg)
{
    handler_global_t *g;
    struct Message *msg;
    struct DosPacket *pkt;
    struct DosPacket *shutdown_pkt = NULL;
    struct FileSysStartupMsg *fssm;
    struct DosEnvec *de;
    ULONG dossig, chgsig, waitmask;
    int running = 1;

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

    /* parse FSSM early so startup failures can log device context */
    {
        int len = AROS_BSTR_strlen(fssm->fssm_Device);
        if (len >= (int)sizeof(g->devname))
            len = sizeof(g->devname) - 1;
        memcpy(g->devname, AROS_BSTR_ADDR(fssm->fssm_Device), len);
        g->devname[len] = '\0';
    }
    g->devunit = fssm->fssm_Unit;
    g->devflags = fssm->fssm_Flags;

    de = (struct DosEnvec *)BADDR(fssm->fssm_Environ);
    g->envec = de;
    g->sector_size = de->de_SizeBlock << 2;

    /* set up logging before any startup error path can fire */
    odfs_log_init(&g->log);
    odfs_log_set_sink(&g->log, log_sink, NULL);
    odfs_log_set_level(&g->log, ODFS_LOG_INFO);
#if ODFS_PACKET_TRACE
    odfs_log_set_level(&g->log, ODFS_LOG_TRACE);
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
    ODFS_INFO(&g->log, ODFS_SUB_CORE, "libraries open, device=%s unit=%lu",
              g->devname, (unsigned long)g->devunit);

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
     * Size: 8 sectors (16KB) — enough for multi-sector reads.
     */
    {
        #define DMA_BUF_SECTORS  8
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
     * on a not-ready unit (unlike HD_SCSICMD, which can hang). A failure
     * here means the unit has no usable device behind it — e.g. the
     * empty/phantom second ATAPI channel that QEMU's peg2ide reports
     * from a floating bus. Decline the mount in that case rather than
     * publishing a dead drive that DOS would route to and poll.
     *
     * When geometry succeeds but reports a non-2048 block size, switch
     * the drive to 2048-byte CD blocks with MODE SELECT (an HD_SCSICMD,
     * issued only for a confirmed present drive so it cannot hang on a
     * phantom unit).
     */
    {
        struct DriveGeometry geom;
        LONG geo_rc;

        memset(&geom, 0, sizeof(geom));
        g->devreq->io_Command = TD_GETGEOMETRY;
        g->devreq->io_Data    = &geom;
        g->devreq->io_Length  = sizeof(geom);
        geo_rc = DoIO((struct IORequest *)g->devreq);
        ODFS_INFO(&g->log, ODFS_SUB_IO,
                  "geometry rc=%ld sector=%lu", (long)geo_rc,
                  (unsigned long)geom.dg_SectorSize);

        if (geo_rc != 0) {
            ODFS_WARN(&g->log, ODFS_SUB_IO,
                      "no usable device on unit %lu (geometry rc=%ld) - "
                      "declining mount",
                      (unsigned long)g->devunit, (long)geo_rc);
            pkt->dp_Res1 = DOSFALSE;
            pkt->dp_Res2 = ERROR_DEVICE_NOT_MOUNTED;
            return_packet(g, pkt);
            goto shutdown;
        }

        if (geom.dg_SectorSize != 0 && geom.dg_SectorSize != 2048) {
            ODFS_INFO(&g->log, ODFS_SUB_IO, "mode select...");
            (void)scsi_mode_select(g, 2048);
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
    mount_volume(g);

    /* install media change interrupt */
    install_media_change(g);

    /* ---- main packet loop ---- */
    dossig = 1UL << g->dosport->mp_SigBit;
    chgsig = (g->chgsigbit >= 0) ? (1UL << g->chgsigbit) : 0;
    waitmask = dossig | chgsig;

    while (running) {
        ULONG sigs = Wait(waitmask);

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

#if ODFS_AMIGA_OS4
                if (g->vector_port) {
                    /*
                     * Route direct legacy packets through the DOS packet
                     * emulator, which performs the equivalent vector-port
                     * call exactly like a native DOS caller would.
                     */
                    odfs_os4_emulate_packet(g->vector_port, pkt);
                    return_packet(g, pkt);
                    continue;
                }
#endif

                if (!g->mounted && packet_needs_live_mount(pkt)) {
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

    /*
     * ---- shutdown ----
     * Invalidate the vector port first so dos.library stops vectoring
     * new callers, then tear down DOS-visible state while holding the
     * filesystem semaphore so in-flight vector calls finish first.
     * The shutdown packet is replied only after the teardown is done.
     */
    ODFS_FS_LOCK(g);
#if ODFS_AMIGA_OS4
    odfs_os4_invalidate_vector_port(g->vector_port);
#endif
    remove_media_change(g);
    unmount_volume(g);
    drain_all_objects(g);
    unpublish_device_node(g);
    ODFS_FS_UNLOCK(g);

    if (shutdown_pkt) {
        shutdown_pkt->dp_Res1 = DOSTRUE;
        shutdown_pkt->dp_Res2 = 0;
        return_packet(g, shutdown_pkt);
    }

shutdown:
    if (g->devreq) {
        if (g->devreq->io_Device)
            CloseDevice((struct IORequest *)g->devreq);
        odfs_amiga_delete_io_request((struct IORequest *)g->devreq);
    }
    if (g->devport)
        odfs_amiga_delete_msg_port(g->devport);

    /* free DMA bounce buffer */
    if (g->dma_buf_raw)
        odfs_amiga_free_mem(g->dma_buf_raw,
                            DMA_BUF_SECTORS * g->sector_size + 15);

    if (g->devnode)
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
