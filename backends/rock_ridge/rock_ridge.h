/*
 * rock_ridge.h — Rock Ridge extension parser
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * References: IEEE P1282 (SUSP), IEEE P1281 (RRIP)
 */

#ifndef ODFS_ROCK_RIDGE_H
#define ODFS_ROCK_RIDGE_H

#include "odfs/node.h"
#include <stdint.h>
#include <stddef.h>

/* System Use Entry header (SUSP) */
#define SUSP_SIG(a, b) (((uint16_t)(a) << 8) | (b))

#define RR_SIG_SP  SUSP_SIG('S','P')  /* SUSP indicator */
#define RR_SIG_CE  SUSP_SIG('C','E')  /* continuation area */
#define RR_SIG_ER  SUSP_SIG('E','R')  /* extensions reference */
#define RR_SIG_ST  SUSP_SIG('S','T')  /* terminator */
#define RR_SIG_RR  SUSP_SIG('R','R')  /* RR indicator (deprecated but common) */
#define RR_SIG_NM  SUSP_SIG('N','M')  /* alternate name */
#define RR_SIG_PX  SUSP_SIG('P','X')  /* POSIX attributes */
#define RR_SIG_TF  SUSP_SIG('T','F')  /* timestamps */
#define RR_SIG_SL  SUSP_SIG('S','L')  /* symbolic link */
#define RR_SIG_CL  SUSP_SIG('C','L')  /* child link */
#define RR_SIG_PL  SUSP_SIG('P','L')  /* parent link */
#define RR_SIG_RE  SUSP_SIG('R','E')  /* relocated entry */

/* NM flags */
#define RR_NM_CONTINUE  0x01  /* name continues in next NM entry */
#define RR_NM_CURRENT   0x02  /* refers to "." */
#define RR_NM_PARENT    0x04  /* refers to ".." */

/* TF flags (which timestamps are present) */
#define RR_TF_CREATION   0x01
#define RR_TF_MODIFY     0x02
#define RR_TF_ACCESS     0x04
#define RR_TF_ATTRIBUTES 0x08
#define RR_TF_BACKUP     0x10
#define RR_TF_EXPIRATION 0x20
#define RR_TF_EFFECTIVE  0x40
#define RR_TF_LONG_FORM  0x80  /* 17-byte timestamps vs 7-byte */

/* SP check bytes */
#define RR_SP_CHECK1  0xBE
#define RR_SP_CHECK2  0xEF

/* parsed RR data to overlay onto an odfs_node_t */
typedef struct rr_info {
    int      has_name;           /* NM entry found */
    char     name[512];          /* alternate name from NM */
    int      has_posix;          /* PX entry found */
    uint32_t mode;               /* POSIX mode from PX */
    uint32_t nlinks;             /* link count from PX */
    uint32_t uid;                /* user ID from PX */
    uint32_t gid;                /* group ID from PX */
    int      has_timestamps;     /* TF entry found */
    odfs_timestamp_t mtime;
    odfs_timestamp_t ctime;
    int      is_symlink;         /* SL entry found */
    char     symlink_target[512];
    int      is_relocated;       /* RE entry found (skip this entry) */
    uint32_t child_link_lba;     /* CL: real location of relocated dir */
    int      has_child_link;
} rr_info_t;

/*
 * Check whether Rock Ridge is present in a directory's "." entry.
 * Returns 1 if SP signature found with correct check bytes, 0 otherwise.
 *   sua       — pointer to System Use Area data
 *   sua_len   — length of System Use Area in bytes
 *   skip_out  — receives the SP skip length (bytes to skip in future entries)
 */
int rr_detect(const uint8_t *sua, size_t sua_len, int *skip_out);

/*
 * Parse Rock Ridge entries from a System Use Area.
 *   sua       — pointer to System Use Area
 *   sua_len   — length in bytes
 *   skip      — bytes to skip at start (from SP entry)
 *   info      — receives parsed RR data
 *   cache     — block cache (for CE continuation reads)
 *   media_ctx — for CE reads (unused if no CE present)
 *
 * CE (continuation area) entries are followed automatically.
 */
struct odfs_cache;
void rr_parse(const uint8_t *sua, size_t sua_len, int skip,
              rr_info_t *info,
              struct odfs_cache *cache);

#endif /* ODFS_ROCK_RIDGE_H */
