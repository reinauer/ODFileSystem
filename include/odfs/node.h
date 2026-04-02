/*
 * odfs/node.h — internal node model
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef ODFS_NODE_H
#define ODFS_NODE_H

#include <stdint.h>
#include <stddef.h>

/* backend type that produced this node */
typedef enum odfs_backend_type {
    ODFS_BACKEND_NONE = 0,
    ODFS_BACKEND_ISO9660,
    ODFS_BACKEND_ROCK_RIDGE,
    ODFS_BACKEND_JOLIET,
    ODFS_BACKEND_UDF,
    ODFS_BACKEND_HFS,
    ODFS_BACKEND_HFSPLUS,
    ODFS_BACKEND_CDDA,
    ODFS_BACKEND__COUNT
} odfs_backend_type_t;

/* node kinds */
typedef enum odfs_node_kind {
    ODFS_NODE_FILE = 0,
    ODFS_NODE_DIR,
    ODFS_NODE_SYMLINK,
    ODFS_NODE_VIRTUAL,    /* e.g. CDDA virtual track */
    ODFS_NODE__COUNT
} odfs_node_kind_t;

/* timestamps */
typedef struct odfs_timestamp {
    int32_t  year;
    uint8_t  month;     /* 1-12 */
    uint8_t  day;       /* 1-31 */
    uint8_t  hour;      /* 0-23 */
    uint8_t  minute;    /* 0-59 */
    uint8_t  second;    /* 0-59 */
    int16_t  tz_offset; /* minutes from UTC, or INT16_MIN if unknown */
} odfs_timestamp_t;

/* extent (contiguous on-disc region) */
typedef struct odfs_extent {
    uint32_t lba;       /* starting logical block address */
    uint32_t length;    /* length in bytes */
} odfs_extent_t;

/* maximum name length for internal representation (UTF-8) */
#define ODFS_NAME_MAX 512

/* internal node */
typedef struct odfs_node {
    uint32_t             id;            /* unique internal id */
    uint32_t             parent_id;     /* parent node id (0 = root) */
    odfs_backend_type_t backend;       /* which backend produced this */
    odfs_node_kind_t    kind;

    char                 name[ODFS_NAME_MAX]; /* UTF-8 normalized name */

    uint64_t             size;          /* file size in bytes */
    odfs_timestamp_t    mtime;         /* modification time */
    odfs_timestamp_t    ctime;         /* creation time */
    uint32_t             mode;          /* unix-style mode bits (from RR etc), 0 if unavailable */

    /* data location */
    odfs_extent_t       extent;        /* primary extent */

    /* backend-private opaque data */
    void                *backend_data;
} odfs_node_t;

const char *odfs_backend_type_name(odfs_backend_type_t type);
const char *odfs_node_kind_name(odfs_node_kind_t kind);

#endif /* ODFS_NODE_H */
