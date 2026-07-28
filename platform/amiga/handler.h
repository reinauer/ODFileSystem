/*
 * handler.h — AmigaDOS handler internal structures
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef ODFS_HANDLER_H
#define ODFS_HANDLER_H

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <exec/interrupts.h>
#include <exec/semaphores.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/filehandler.h>

#include "amiga_target_compat.h"
#include "aros_compat.h"
#include "odfs/api.h"

typedef struct odfs_volume odfs_volume_t;
typedef struct odfs_entry odfs_entry_t;
typedef struct odfs_lock odfs_lock_t;
typedef struct odfs_fh odfs_fh_t;
typedef struct odfs_changeint_data odfs_changeint_data_t;
#if ODFS_AMIGA_OS4
struct FileSystemVectorPort;
#endif

struct odfs_changeint_data {
    struct Task       *task;
    ULONG              sigmask;
};

/* Filesystem DOSType (matches the OS4 FileSysEntry). */
#define ODFS_OS4_CD_DOSTYPE 0x43443031UL /* 'CD01' */

/*
 * Media-adapter context passed to the odfs_media_ops callbacks. It must
 * be per-handler-process: diskboot starts one handler process per CD
 * unit, so a shared instance would let a later process clobber the
 * device binding of an earlier one. It lives inside handler_global so
 * each process owns its own copy.
 */
struct handler_global;
typedef struct amiga_media_ctx {
    struct handler_global *g;
} amiga_media_ctx_t;

#if !ODFS_AMIGA_OS4
typedef struct odfs_exnext_cursor {
    ULONG    dir_key;
    ULONG    previous_key;
    uint32_t resume;
    int      valid;
    int      cdda_emitted;
} odfs_exnext_cursor_t;
#endif

/* ---- handler globals ---- */

typedef struct handler_global {
    struct MsgPort      *process_port;  /* owning process message port */
    struct MsgPort      *dosport;       /* DOS message port */
#if ODFS_AMIGA_OS4
    struct Task         *handler_task;  /* task that owns handler ports */
    struct Task         *vector_io_task; /* task owning cached vector I/O */
    struct MsgPort      *vector_io_port; /* cached caller-task reply port */
    struct IOStdReq     *vector_io_req;  /* cached caller-task I/O request */
    struct FileSystemVectorPort *vector_port; /* native OS4 vector port */
    LONG                 vector_sigbit; /* signal bit used by vector port */
    /*
     * OS4 vector callbacks run in the calling process context, so all
     * access to handler state from the vectors and from the handler
     * process must hold this semaphore.
     */
    struct SignalSemaphore fs_sem;
#endif
    amiga_media_ctx_t    media_ctx;     /* per-process media-adapter ctx */
    struct DeviceNode   *devnode;       /* startup packet device node */
    struct FileSysStartupMsg *fssm;     /* startup packet FSSM */
    struct DeviceNode   *published_devnode; /* DOS device-list entry */
    int                  published_devnode_owned; /* published_devnode allocated by us */
    struct DeviceList   *volnode;       /* DOS volume node (or NULL) */
    odfs_volume_t       *current_volume;/* current mounted volume state */

    /* libraries */
    struct ExecBase     *sysbase;
    struct DosLibrary   *dosbase;

    /* device I/O */
    struct MsgPort      *devport;       /* device I/O port */
    struct IOStdReq     *devreq;        /* device I/O request */
    ULONG                sector_size;   /* device block size */
    char                 devname[128];  /* device name */
    ULONG                devunit;       /* device unit */
    ULONG                devflags;      /* device open flags */
    struct DosEnvec     *envec;         /* for control string parsing */

    /* DMA-safe bounce buffer */
    /* free-list pools for per-packet objects (entry/lock/fh) */
    void                *entry_pool;
    void                *lock_pool;
    void                *fh_pool;

    uint8_t             *dma_buf_raw;   /* raw allocation (for FreeMem) */
    uint8_t             *dma_buf;       /* 16-byte aligned bounce buffer */
    ULONG                dma_buf_size;  /* usable size in bytes */
#if !ODFS_AMIGA_OS4
    uint8_t             *direct_read_buf; /* ACTION_READ buffer, or NULL */
    ULONG                direct_read_len; /* allowed direct-read bytes */
#endif

    /* filesystem state */
    odfs_mount_t        mount;
    odfs_media_t        media;
    odfs_log_state_t    log;
    int                  mounted;       /* filesystem mounted? */
    int                  inhibited;     /* ACTION_INHIBIT active? */
    int                  toc_passthrough; /* -1 unknown, 0 unsupported, 1 ok */
    int                  last_session_passthrough; /* -1 unknown, 0 unsupported, 1 ok */
    int                  read_cd_audio;   /* -1 unknown, 0 unsupported, 1 ok */
    int                  read64_mode;     /* how to read past 4 GiB: see ODFS_READ64_* */
#define ODFS_READ64_UNPROBED 0
#define ODFS_READ64_TD64     1   /* TD_READ64 (trackdisk64) */
#define ODFS_READ64_NSD      2   /* NSCMD_TD_READ64 (new style device) */
#define ODFS_READ64_SCSI     3   /* SCSI READ(10) via HD_SCSICMD */
#define ODFS_READ64_NONE     (-1)
    int                  cdtext_passthrough; /* -1 unknown, 0 unsupported, 1 ok */

    /* media change */
    struct MsgPort      *chgport;       /* media change signal port */
    struct IOStdReq     *chgreq;        /* media change I/O request */
    struct Interrupt     changeint;     /* TD_ADDCHANGEINT callback */
    odfs_changeint_data_t changeint_data; /* callback payload */
    LONG                 chgsigbit;     /* signal bit for media change */
    int                  chg_installed; /* TD_CHANGEINT installed? */
    ULONG                change_count;  /* last observed TD_CHANGENUM */
    int                  change_count_valid; /* change_count initialized? */
    int                  geo_pending;   /* re-probe geometry once media shows up */

    /* deferred DOS volume-list publication */
    struct MsgPort      *publish_timer_port;
    struct IORequest    *publish_timer_req;
    ULONG                publish_retry_count;
    int                  publish_timer_open;
    int                  publish_timer_pending;

    /* volume info */
    char                 volname[128];  /* volume name */

    /* CDDA */
    void                *cdda_ctx;      /* cdda_context_t* if audio tracks present */
    odfs_node_t         cdda_root;     /* CDDA virtual dir node */
    int                  has_cdda;      /* audio tracks detected */

#if !ODFS_AMIGA_OS4
    odfs_exnext_cursor_t root_exnext;   /* null-lock root ExNext cursor */
#endif

    /* lock list */
    struct MinList       locklist;      /* active locks */
    struct MinList       fhlist;        /* active file handles */
    struct MinList       volumes;       /* current and stale volumes */
    ULONG                next_volume_id;/* next volume generation */
} handler_global_t;

/* ---- volume tracking ---- */

struct odfs_volume {
    struct MinNode      node;
    struct DeviceList  *volnode;
    odfs_lock_t        *lock_head;
    ULONG               id;
    ULONG               object_count;
    int                 listed;         /* volnode is in the DOS list */
};

/* ---- object metadata shared by locks and filehandles ---- */

struct odfs_entry {
    odfs_volume_t      *volume;
    odfs_node_t         fnode;
    odfs_node_t         parent_node;
    odfs_node_t         grandparent_node;
    int                 has_grandparent;
    ULONG               refcount;
};

/* ---- lock wrapper ---- */

struct odfs_lock {
    struct MinNode  node;           /* for locklist */
#if ODFS_AMIGA_OS4
    struct Lock    *lock;           /* DOS-allocated OS4 lock */
#else
    struct FileLock lock;           /* DOS lock (MUST be at known offset) */
    ULONG           dos_private[2]; /* reserve fl_SIZEOF..fl_SIZEOF+7 for DOS */
    odfs_exnext_cursor_t exnext;     /* handler-owned ExNext resume cursor */
#endif
    odfs_entry_t  *entry;          /* shared object metadata */
    ULONG           key;            /* unique key */
    ULONG           magic;          /* liveness check for BPTR validation */
    odfs_lock_t    *volume_prev;    /* per-volume DOS lock chain */
    odfs_lock_t    *volume_next;    /* per-volume DOS lock chain */
};

#if !ODFS_AMIGA_OS4
typedef char odfs_lock_private_offset_must_match[
    (offsetof(odfs_lock_t, dos_private) ==
     offsetof(odfs_lock_t, lock) + sizeof(struct FileLock)) ? 1 : -1
];
typedef char odfs_lock_private_size_must_match[
    (sizeof(((odfs_lock_t *)0)->dos_private) == 8) ? 1 : -1
];
#endif

/* ---- file handle wrapper ---- */

struct odfs_fh {
    struct MinNode  node;           /* for tracking */
    odfs_entry_t  *entry;          /* shared object metadata */
    LONG            access;         /* originating DOS access mode */
    uint64_t        pos;            /* current read position */
    ULONG           magic;          /* liveness check for handle validation */
};

/* ---- helper macros ---- */

#if ODFS_AMIGA_OS4
#define ODFS_LOCK_DOS(ol) \
    (((ol) != NULL) ? (ol)->lock : NULL)

/* Convert a direct DOS lock pointer to our odfs_lock_t */
#define LOCK_FROM_PTR(ptr) \
    ((ptr) ? (odfs_lock_t *)((struct Lock *)(ptr))->fl_FSPrivate1 : NULL)

/* Convert BPTR lock to our odfs_lock_t */
#define LOCK_FROM_BPTR(bptr) \
    ((bptr) ? LOCK_FROM_PTR((struct Lock *)BADDR(bptr)) : NULL)

/* Convert odfs_lock_t to BPTR for DOS */
#define LOCK_TO_BPTR(ol) \
    (((ol) && (ol)->lock) ? MKBADDR((ol)->lock) : 0)

/* Convert odfs_lock_t to a direct DOS lock pointer */
#define LOCK_TO_PTR(ol) \
    (((ol) && (ol)->lock) ? (ol)->lock : NULL)
#else
#define ODFS_LOCK_DOS(ol) \
    (((ol) != NULL) ? &(ol)->lock : NULL)

/* Convert BPTR lock to our odfs_lock_t */
#define LOCK_FROM_BPTR(bptr) \
    ((bptr) ? (odfs_lock_t *)((UBYTE *)BADDR(bptr) - \
     offsetof(odfs_lock_t, lock)) : NULL)

/* Convert odfs_lock_t to BPTR for DOS */
#define LOCK_TO_BPTR(ol) \
    ((ol) ? MKBADDR(&(ol)->lock) : 0)

/* Convert a direct DOS lock pointer to our odfs_lock_t */
#define LOCK_FROM_PTR(ptr) \
    ((ptr) ? (odfs_lock_t *)((UBYTE *)(ptr) - \
     offsetof(odfs_lock_t, lock)) : NULL)

/* Convert odfs_lock_t to a direct DOS lock pointer */
#define LOCK_TO_PTR(ol) \
    ((ol) ? &(ol)->lock : NULL)
#endif

/* BCPL string to C string (AROS-compatible) */
static inline void bstr_to_cstr(BSTR bstr, char *buf, int bufsize)
{
    if (!buf || bufsize <= 0)
        return;
    if (!bstr) {
        buf[0] = '\0';
        return;
    }

    int len = AROS_BSTR_strlen(bstr);
    const char *addr = (const char *)AROS_BSTR_ADDR(bstr);
    if (len >= bufsize)
        len = bufsize - 1;
    for (int i = 0; i < len; i++)
        buf[i] = addr[i];
    buf[len] = '\0';
}

/* Map odfs error to DOS error code */
static inline LONG odfs_err_to_dos(odfs_err_t err)
{
    switch (err) {
    case ODFS_OK:              return 0;
    case ODFS_ERR_NOMEM:       return ERROR_NO_FREE_STORE;
    case ODFS_ERR_IO:          return ERROR_SEEK_ERROR;
    case ODFS_ERR_INVAL:       return ERROR_BAD_NUMBER;
    case ODFS_ERR_RANGE:       return ERROR_SEEK_ERROR;
    case ODFS_ERR_OVERFLOW:    return ERROR_SEEK_ERROR;
    case ODFS_ERR_NO_MEDIA:    return ERROR_NO_DISK;
    case ODFS_ERR_BAD_SECTOR:  return ERROR_SEEK_ERROR;
    case ODFS_ERR_MEDIA_CHANGED: return ERROR_DEVICE_NOT_MOUNTED;
    case ODFS_ERR_NOT_FOUND:   return ERROR_OBJECT_NOT_FOUND;
    case ODFS_ERR_BAD_FORMAT:  return ERROR_NOT_A_DOS_DISK;
    case ODFS_ERR_UNSUPPORTED: return ERROR_ACTION_NOT_KNOWN;
    case ODFS_ERR_CORRUPT:     return ERROR_NOT_A_DOS_DISK;
    case ODFS_ERR_LOOP:        return ERROR_TOO_MANY_LEVELS;
    case ODFS_ERR_NAME_TOO_LONG: return ERROR_LINE_TOO_LONG;
    case ODFS_ERR_NOT_DIR:     return ERROR_OBJECT_WRONG_TYPE;
    case ODFS_ERR_IS_DIR:      return ERROR_OBJECT_WRONG_TYPE;
    case ODFS_ERR_IS_SYMLINK:  return ERROR_IS_SOFT_LINK;
    case ODFS_ERR_READ_ONLY:   return ERROR_DISK_WRITE_PROTECTED;
    case ODFS_ERR_TOO_MANY_OPEN: return ERROR_TOO_MANY_LEVELS;
    case ODFS_ERR_EOF:         return 0;
    default:                    return ERROR_NOT_A_DOS_DISK;
    }
}

typedef struct odfs_handler_node_info {
    const char      *name;
    const char      *comment;
    ULONG            key;
    ULONG            protection;
    uint64_t         size;
    struct DateStamp date;
    LONG             fib_type;
    int              is_dir;
} odfs_handler_node_info_t;

/* shared operations used by packet and OS4 vector frontends */
void odfs_handler_fill_node_info(handler_global_t *g,
                                 const odfs_node_t *node,
                                 odfs_handler_node_info_t *info);

/*
 * Resolve the soft link that `path` (relative to parent_lock, or the
 * volume root when parent_lock is NULL) stopped on, writing the target
 * in AmigaDOS path syntax — with any unconsumed path remainder
 * re-appended — into buf. Shared by the packet ACTION_READ_LINK handler
 * and the OS4 FSReadSoftLink vector. Returns the target length in bytes
 * (excluding the terminating NUL) on success, -1 on error, or -2 when
 * buf is too small; *err_out receives the DOS error code for the -1/-2
 * cases and 0 on success.
 */
LONG odfs_handler_read_soft_link(handler_global_t *g,
                                 odfs_lock_t *parent_lock,
                                 const char *path,
                                 char *buf, LONG bufsize,
                                 LONG *err_out);
#if ODFS_AMIGA_OS4
LONG odfs_handler_resolve_object_node(handler_global_t *g,
                                      odfs_lock_t *parent_lock,
                                      const char *path,
                                      odfs_node_t *node_out,
                                      odfs_node_t *parent_out);
#endif
LONG odfs_handler_lock_object(handler_global_t *g,
                              odfs_lock_t *parent_lock,
                              const char *path,
                              LONG access,
                              odfs_lock_t **out);
LONG odfs_handler_free_lock_object(handler_global_t *g, odfs_lock_t *ol);
LONG odfs_handler_dup_lock_object(handler_global_t *g,
                                  odfs_lock_t *src,
                                  odfs_lock_t **out);
LONG odfs_handler_dup_lock_from_fh(handler_global_t *g,
                                   odfs_fh_t *fh,
                                   odfs_lock_t **out);
LONG odfs_handler_parent_lock_object(handler_global_t *g,
                                     odfs_lock_t *ol,
                                     odfs_lock_t **out);
LONG odfs_handler_parent_fh_object(handler_global_t *g,
                                   odfs_fh_t *fh,
                                   odfs_lock_t **out);
LONG odfs_handler_same_lock_object(handler_global_t *g,
                                   odfs_lock_t *l1,
                                   odfs_lock_t *l2,
                                   LONG *same_result);
LONG odfs_handler_same_file_object(handler_global_t *g,
                                   odfs_fh_t *fh1,
                                   odfs_fh_t *fh2,
                                   LONG *same_result);
LONG odfs_handler_open_object(handler_global_t *g,
                              odfs_lock_t *dirlock,
                              const char *path,
                              LONG mode,
                              odfs_fh_t **out);
LONG odfs_handler_open_from_lock_object(handler_global_t *g,
                                        odfs_lock_t *ol,
                                        odfs_fh_t **out);
LONG odfs_handler_close_object(handler_global_t *g, odfs_fh_t *fh);
LONG odfs_handler_read_object(handler_global_t *g,
                              odfs_fh_t *fh,
                              void *buf,
                              LONG len,
                              LONG *actual_out);
LONG odfs_handler_seek_object(handler_global_t *g,
                              odfs_fh_t *fh,
                              int64_t offset,
                              LONG mode,
                              int64_t *oldpos_out);
LONG odfs_handler_change_lock_mode(handler_global_t *g,
                                   odfs_lock_t *ol,
                                   LONG mode);
LONG odfs_handler_change_file_mode(handler_global_t *g,
                                   odfs_fh_t *fh,
                                   LONG mode);
LONG odfs_handler_get_file_position(handler_global_t *g,
                                    odfs_fh_t *fh,
                                    int64_t *pos_out);
LONG odfs_handler_get_file_size(handler_global_t *g,
                                odfs_fh_t *fh,
                                int64_t *size_out);
LONG odfs_handler_fill_info(handler_global_t *g,
                            odfs_lock_t *ol,
                            struct InfoData *info);
LONG odfs_handler_get_lock_node(handler_global_t *g,
                                odfs_lock_t *ol,
                                const odfs_node_t **node_out);
LONG odfs_handler_get_fh_node(handler_global_t *g,
                              odfs_fh_t *fh,
                              const odfs_node_t **node_out);
LONG odfs_handler_next_dir_entry(handler_global_t *g,
                                 odfs_lock_t *ol,
                                 ULONG previous_key,
                                 uint32_t *resume_io,
                                 odfs_node_t *entry_out,
                                 ULONG *key_out);
LONG odfs_handler_inhibit(handler_global_t *g, LONG state);

/* handler entry point (called from startup.S) */
void handler_main(void);
void handler_main_startup(struct Message *startup_msg);

#endif /* ODFS_HANDLER_H */
