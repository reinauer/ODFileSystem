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

/* ---- handler globals ---- */

typedef struct handler_global {
    struct MsgPort      *process_port;  /* owning process message port */
    struct MsgPort      *dosport;       /* DOS message port */
#if ODFS_AMIGA_OS4
    struct FileSystemVectorPort *vector_port; /* native OS4 vector port */
    LONG                 vector_sigbit; /* signal bit used by vector port */
#endif
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
    uint8_t             *dma_buf_raw;   /* raw allocation (for FreeMem) */
    uint8_t             *dma_buf;       /* 16-byte aligned bounce buffer */
    ULONG                dma_buf_size;  /* usable size in bytes */

    /* filesystem state */
    odfs_mount_t        mount;
    odfs_media_t        media;
    odfs_log_state_t    log;
    int                  mounted;       /* filesystem mounted? */
    int                  inhibited;     /* ACTION_INHIBIT active? */
    int                  toc_passthrough; /* -1 unknown, 0 unsupported, 1 ok */
    int                  last_session_passthrough; /* -1 unknown, 0 unsupported, 1 ok */
    int                  read_cd_audio;   /* -1 unknown, 0 unsupported, 1 ok */
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

    /* volume info */
    char                 volname[128];  /* volume name */

    /* CDDA */
    void                *cdda_ctx;      /* cdda_context_t* if audio tracks present */
    odfs_node_t         cdda_root;     /* CDDA virtual dir node */
    int                  has_cdda;      /* audio tracks detected */

    /* lock list */
    struct MinList       locklist;      /* active locks */
    struct MinList       fhlist;        /* active file handles */
    ULONG                next_volume_id;/* next volume generation */
} handler_global_t;

/* ---- volume tracking ---- */

struct odfs_volume {
    struct MinNode      node;
    struct DeviceList  *volnode;
    ULONG               id;
    ULONG               object_count;
};

/* ---- object metadata shared by locks and filehandles ---- */

struct odfs_entry {
    odfs_volume_t      *volume;
    odfs_node_t         fnode;
    odfs_node_t         parent_node;
    ULONG               refcount;
};

/* ---- lock wrapper ---- */

struct odfs_lock {
    struct MinNode  node;           /* for locklist */
    struct FileLock lock;           /* DOS lock (MUST be at known offset) */
    ULONG           dos_private[2]; /* reserve fl_SIZEOF..fl_SIZEOF+7 for DOS */
    odfs_entry_t  *entry;          /* shared object metadata */
    ULONG           key;            /* unique key */
};

typedef char odfs_lock_private_offset_must_match[
    (offsetof(odfs_lock_t, dos_private) ==
     offsetof(odfs_lock_t, lock) + sizeof(struct FileLock)) ? 1 : -1
];
typedef char odfs_lock_private_size_must_match[
    (sizeof(((odfs_lock_t *)0)->dos_private) == 8) ? 1 : -1
];

/* ---- file handle wrapper ---- */

struct odfs_fh {
    struct MinNode  node;           /* for tracking */
    odfs_entry_t  *entry;          /* shared object metadata */
    LONG            access;         /* originating DOS access mode */
    uint64_t        pos;            /* current read position */
};

/* ---- helper macros ---- */

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
    case ODFS_ERR_READ_ONLY:   return ERROR_DISK_WRITE_PROTECTED;
    case ODFS_ERR_TOO_MANY_OPEN: return ERROR_TOO_MANY_LEVELS;
    case ODFS_ERR_EOF:         return 0;
    default:                    return ERROR_NOT_A_DOS_DISK;
    }
}

/* shared operations used by packet and OS4 vector frontends */
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
                                 odfs_node_t *entry_out,
                                 ULONG *key_out);
ULONG odfs_handler_node_key(const odfs_node_t *node);
ULONG odfs_handler_node_protection(const odfs_node_t *node);
void odfs_handler_node_date(const odfs_node_t *node, struct DateStamp *ds);
LONG odfs_handler_inhibit(handler_global_t *g, LONG state);

/* handler entry point (called from startup.S) */
void handler_main(void);
void handler_main_startup(struct Message *startup_msg);

#endif /* ODFS_HANDLER_H */
