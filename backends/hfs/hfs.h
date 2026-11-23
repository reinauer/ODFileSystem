/*
 * hfs.h — HFS (Hierarchical File System) on-disc structures
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * References: Inside Macintosh: Files, Linux fs/hfs/hfs.h
 */

#ifndef ODFS_HFS_H
#define ODFS_HFS_H

#include "odfs/backend.h"
#include "hfs/hfs_common.h"
#include <stdint.h>

/* ---- MDB (Master Directory Block) ---- */

#define HFS_MDB_SIG          0x4244   /* "BD" big-endian */
#define HFS_MDB_OFFSET_512   2        /* 512-byte block 2 from partition start */

/* MDB field offsets (all big-endian) */
#define HFS_MDB_SIGWORD       0    /* uint16 */
#define HFS_MDB_CRDATE        2    /* uint32 */
#define HFS_MDB_LSMOD         6    /* uint32 */
#define HFS_MDB_NMFLS        12    /* uint16: files in root */
#define HFS_MDB_NMALBLKS     18    /* uint16: allocation block count */
#define HFS_MDB_ALBLKSIZ     20    /* uint32: allocation block size */
#define HFS_MDB_ALBLST       28    /* uint16: first alloc block (512-blk offset) */
#define HFS_MDB_FREEBKS      34    /* uint16: free blocks */
#define HFS_MDB_VOLNAMELEN   36    /* uint8: volume name length */
#define HFS_MDB_VOLNAME      37    /* 27 bytes: volume name (Mac Roman) */
#define HFS_MDB_NMRTDIRS     82    /* uint16: dirs in root */
#define HFS_MDB_FILCNT       84    /* uint32: file count */
#define HFS_MDB_DIRCNT       88    /* uint32: directory count */
#define HFS_MDB_EMBEDSIG    124    /* uint16: embedded HFS+ signature */
#define HFS_MDB_EMBEDEXT    126    /* 4 bytes: embedded volume extent */
#define HFS_MDB_XTFLSIZE    130    /* uint32: extents B-tree file size */
#define HFS_MDB_XTEXTREC    134    /* 12 bytes: extents B-tree extents */
#define HFS_MDB_CTFLSIZE    146    /* uint32: catalog B-tree file size */
#define HFS_MDB_CTEXTREC    150    /* 12 bytes: catalog B-tree extents */

/* volume_name buffer size, NUL included */

#define HFS_VOLNAME_SIZE      28

extern const odfs_backend_ops_t hfs_backend_ops;

#endif /* ODFS_HFS_H */
