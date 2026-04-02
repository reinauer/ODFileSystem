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
#include <workbench/startup.h>
#include <workbench/workbench.h>

#include "odfs/api.h"

#define ODFS_HANDLER_VERSION  "0.1"

/* ---- handler globals ---- */

typedef struct handler_global {
    struct MsgPort      *dosport;       /* DOS message port */
    struct DeviceNode   *devnode;       /* our device node */
    struct DeviceList   *volnode;       /* DOS volume node (or NULL) */

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

    /* media change */
    struct MsgPort      *chgport;       /* media change signal port */
    struct IOStdReq     *chgreq;        /* media change I/O request */
    LONG                 chgsigbit;     /* signal bit for media change */
    int                  chg_installed; /* TD_CHANGEINT installed? */

    /* volume info */
    char                 volname[128];  /* volume name */

    /* CDDA */
    void                *cdda_ctx;      /* cdda_context_t* if audio tracks present */
    odfs_node_t         cdda_root;     /* CDDA virtual dir node */
    int                  has_cdda;      /* audio tracks detected */

    /* Workbench AppIcon */
    struct Library      *iconbase;      /* icon.library */
    struct Library      *wbbase;        /* workbench.library */
    struct MsgPort      *appport;       /* AppIcon message port */
    struct AppIcon      *appicon;       /* active AppIcon (or NULL) */
    struct DiskObject   *diskobj;       /* icon image */

    /* lock list */
    struct MinList       locklist;      /* active locks */
    ULONG                next_key;      /* next unique lock key */
} handler_global_t;

/* ---- lock wrapper ---- */

typedef struct odfs_lock {
    struct MinNode  node;           /* for locklist */
    struct FileLock lock;           /* DOS lock (MUST be at known offset) */
    odfs_node_t    fnode;          /* filesystem node */
    odfs_node_t    parent_node;    /* parent directory node (for ACTION_PARENT) */
    ULONG           key;            /* unique key */
} odfs_lock_t;

/* ---- file handle wrapper ---- */

typedef struct odfs_fh {
    struct MinNode  node;           /* for tracking */
    odfs_node_t    fnode;          /* filesystem node */
    ULONG           pos;            /* current read position */
} odfs_fh_t;

/* ---- helper macros ---- */

/* Convert BPTR lock to our odfs_lock_t */
#define LOCK_FROM_BPTR(bptr) \
    ((bptr) ? (odfs_lock_t *)((UBYTE *)BADDR(bptr) - \
     offsetof(odfs_lock_t, lock)) : NULL)

/* Convert odfs_lock_t to BPTR for DOS */
#define LOCK_TO_BPTR(ol) \
    ((ol) ? MKBADDR(&(ol)->lock) : 0)

/* BCPL string to C string */
static inline void bstr_to_cstr(BSTR bstr, char *buf, int bufsize)
{
    const UBYTE *bs = (const UBYTE *)BADDR(bstr);
    int len = bs[0];
    if (len >= bufsize)
        len = bufsize - 1;
    for (int i = 0; i < len; i++)
        buf[i] = bs[i + 1];
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

/* handler entry point (called from startup.S) */
void handler_main(void);

#endif /* ODFS_HANDLER_H */
