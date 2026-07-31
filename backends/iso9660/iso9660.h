/*
 * iso9660.h — ISO 9660 on-disc structures and backend interface
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * References: ECMA-119 / ISO 9660:1988
 */

#ifndef ODFS_ISO9660_H
#define ODFS_ISO9660_H

#include "odfs/backend.h"
#include <stdint.h>

/* --- on-disc constants --- */

#define ISO_SECTOR_SIZE       2048
#define ISO_VD_START_LBA      16
#define ISO_VD_TYPE_BOOT      0
#define ISO_VD_TYPE_PRIMARY   1
#define ISO_VD_TYPE_SUPPL     2
#define ISO_VD_TYPE_PARTITION 3
#define ISO_VD_TYPE_TERM      255

#define ISO_STANDARD_ID       "CD001"
#define ISO_STANDARD_ID_LEN   5

#define ISO_MAX_DIR_DEPTH     256   /* loop/recursion guard */

/* --- both-byte order helpers (ECMA-119 7.3.3) --- */

/* read little-endian 32 from a both-byte-order 8-byte field */
static inline uint32_t iso_read_le32(const uint8_t *p)
{
    return (uint32_t)p[0]       | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* read little-endian 16 from a both-byte-order 4-byte field */
static inline uint16_t iso_read_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* copy a fixed-length string field, trimming trailing spaces and NUL */
static inline void iso_copy_strfield(const uint8_t *src, size_t src_len,
                                     char *dst, size_t dst_size)
{
    if (dst_size == 0)
        return;

    size_t len = src_len;
    while (len > 0 && (src[len - 1] == ' ' || src[len - 1] == '\0'))
        len--;
    if (len >= dst_size)
        len = dst_size - 1;
    for (size_t i = 0; i < len; i++)
        dst[i] = (char)src[i];
    dst[len] = '\0';
}

/* --- Primary Volume Descriptor (ECMA-119 8.4) --- */

/*
 * We use byte offsets rather than a packed struct to avoid
 * alignment and packing portability issues.
 */

#define ISO_PVD_TYPE              0     /* 1 byte  */
#define ISO_PVD_ID                1     /* 5 bytes "CD001" */
#define ISO_PVD_VERSION           6     /* 1 byte  */
#define ISO_PVD_SYSTEM_ID         8     /* 32 bytes a-chars */
#define ISO_PVD_VOLUME_ID        40     /* 32 bytes d-chars */
#define ISO_PVD_VOLUME_SPACE_SIZE 80    /* 8 bytes 7.3.3 */
#define ISO_PVD_SET_SIZE         120    /* 4 bytes 7.3.3 */
#define ISO_PVD_SEQ_NUM          124    /* 4 bytes 7.3.3 */
#define ISO_PVD_LOGICAL_BLK_SIZE 128    /* 4 bytes 7.3.3 */
#define ISO_PVD_PATH_TABLE_SIZE  132    /* 8 bytes 7.3.3 */
#define ISO_PVD_PATH_TABLE_LE    140    /* 4 bytes LE */
#define ISO_PVD_PATH_TABLE_OPT_LE 144  /* 4 bytes LE */
#define ISO_PVD_PATH_TABLE_BE    148    /* 4 bytes BE */
#define ISO_PVD_PATH_TABLE_OPT_BE 152  /* 4 bytes BE */
#define ISO_PVD_ROOT_DIR_RECORD  156    /* 34 bytes */
#define ISO_PVD_VOLUME_SET_ID    190    /* 128 bytes */
#define ISO_PVD_PUBLISHER_ID     318    /* 128 bytes */
#define ISO_PVD_PREPARER_ID      446    /* 128 bytes */
#define ISO_PVD_APPLICATION_ID   574    /* 128 bytes */
#define ISO_PVD_CREATION_DATE    813    /* 17 bytes */
#define ISO_PVD_MODIFICATION_DATE 830   /* 17 bytes */
#define ISO_PVD_EXPIRATION_DATE  847    /* 17 bytes */
#define ISO_PVD_EFFECTIVE_DATE   864    /* 17 bytes */

/* --- Directory Record (ECMA-119 9.1) --- */

#define ISO_DR_LENGTH             0     /* 1 byte: total record length */
#define ISO_DR_EXT_ATTR_LENGTH    1     /* 1 byte  */
#define ISO_DR_EXTENT_LBA         2     /* 8 bytes 7.3.3 */
#define ISO_DR_DATA_LENGTH       10     /* 8 bytes 7.3.3 */
#define ISO_DR_DATE              18     /* 7 bytes recording date/time */
#define ISO_DR_FLAGS             25     /* 1 byte  */
#define ISO_DR_UNIT_SIZE         26     /* 1 byte  */
#define ISO_DR_INTERLEAVE_GAP    27     /* 1 byte  */
#define ISO_DR_VOLUME_SEQ_NUM    28     /* 4 bytes 7.3.3 */
#define ISO_DR_NAME_LEN          32     /* 1 byte  */
#define ISO_DR_NAME              33     /* variable */

/* --- High Sierra (pre-ISO 9660, 1986) --- */

/*
 * High Sierra volume descriptors carry an 8-byte both-endian descriptor LBN
 * before the type byte, so every header field sits 8 bytes later than in
 * ISO 9660, and the identifier is "CDROM" instead of "CD001". Directory
 * records share the ISO layout except for a 6-byte recording date (no
 * timezone byte) which puts the flags byte at offset 24 instead of 25.
 */

#define HS_STANDARD_ID           "CDROM"
#define HS_STANDARD_ID_LEN       5

#define HS_VD_TYPE               8     /* 1 byte  */
#define HS_VD_ID                 9     /* 5 bytes "CDROM" */
#define HS_VD_VERSION           14     /* 1 byte  */
#define HS_PVD_SYSTEM_ID        16     /* 32 bytes */
#define HS_PVD_VOLUME_ID        48     /* 32 bytes */
#define HS_PVD_VOLUME_SPACE_SIZE 88    /* 8 bytes 7.3.3 */
#define HS_PVD_LOGICAL_BLK_SIZE 136    /* 4 bytes 7.3.3 */
#define HS_PVD_PATH_TABLE_SIZE  140    /* 8 bytes 7.3.3 */
#define HS_PVD_ROOT_DIR_RECORD  180    /* 34 bytes */

#define HS_DR_DATE              18     /* 6 bytes, no timezone */
#define HS_DR_FLAGS             24     /* 1 byte  */

/* directory record flag bits */
#define ISO_DR_FLAG_HIDDEN       0x01
#define ISO_DR_FLAG_DIRECTORY    0x02
#define ISO_DR_FLAG_ASSOCIATED   0x04
#define ISO_DR_FLAG_RECORD       0x08
#define ISO_DR_FLAG_PROTECTION   0x10
#define ISO_DR_FLAG_MULTI_EXTENT 0x80

/* --- parsed PVD (host representation) --- */

typedef struct iso_pvd_info {
    char     system_id[33];
    char     volume_id[33];
    uint32_t volume_space_size;   /* in logical blocks */
    uint16_t logical_block_size;
    uint32_t path_table_size;
    uint32_t root_dir_lba;
    uint32_t root_dir_size;
    uint8_t  root_dir_record[34]; /* raw root directory record */
} iso_pvd_info_t;

/* --- ISO9660 mount context --- */

/*
 * Shared by the ISO 9660 and Joliet backends: a Joliet mount is an ISO
 * context with the joliet flag set (UCS-2 names, no Rock Ridge, no High
 * Sierra) whose pvd fields were parsed from the SVD. Every function that
 * receives a backend_ctx from either ops table casts to this type, so
 * both backends must allocate exactly this struct.
 */
typedef struct iso_context {
    iso_pvd_info_t pvd;
    uint32_t       session_start;
    uint32_t       next_node_id;
    int            lowercase;     /* lowercase ISO names for display */
    int            high_sierra;   /* High Sierra variant, not ISO 9660 */
    int            joliet;        /* Joliet mount: UCS-2 names, no RR/HS */
    int            has_rock_ridge; /* Rock Ridge extensions detected */
    int            rr_skip;       /* SP skip bytes for RR entries */
    odfs_node_t    root;          /* verbatim root node, for parent resolution */
} iso_context_t;

/* --- shared ancestry helpers (used by iso9660 and joliet) --- */

/*
 * Read the parent directory's extent location from the ".." record at the
 * start of the directory extent at dir_lba (a media LBA). On success returns
 * the parent's media LBA and on-disc data length. Both ISO9660 and Joliet
 * share the ECMA-119 directory-record layout, so this works for either.
 */
odfs_err_t odfs_iso_read_parent_extent(odfs_cache_t *cache,
                                       uint32_t session_start,
                                       uint32_t dir_lba,
                                       uint32_t *parent_lba_out,
                                       uint32_t *parent_size_out);

/* synthesize the minimal directory node needed to enumerate an extent */
odfs_node_t odfs_iso_dir_stub(odfs_backend_type_t backend,
                              uint32_t lba, uint32_t size);

/*
 * Merge the continuation records of a multi-extent file (ISO 9660 Level 3,
 * ECMA-119 6.5.1). Called after parsing a directory record whose
 * multi-extent flag is set; first_rec must still point at that raw record.
 * Consumes every continuation record by advancing *offset past it and
 * accumulates their data lengths into node->size. Only contiguous extents
 * can be represented by the single-extent node model; when a part is
 * non-contiguous the file is truncated to the contiguous prefix and a
 * warning is logged. Shared by the ISO 9660 (plain and High Sierra) and
 * Joliet backends — the record fields involved sit at identical offsets.
 */
odfs_err_t odfs_iso_merge_multi_extent(odfs_cache_t *cache,
                                       odfs_log_state_t *log,
                                       uint32_t session_start,
                                       uint32_t dir_lba,
                                       uint32_t dir_size,
                                       uint32_t *offset,
                                       int high_sierra,
                                       const uint8_t *first_rec,
                                       odfs_node_t *node);

/* --- shared backend ops (used by the iso9660 and joliet ops tables) --- */

/*
 * Mount the volume descriptor at vd_lba (a media LBA) into a fresh
 * iso_context_t. With joliet set, the descriptor is treated as a Joliet
 * SVD: the volume id is decoded from UCS-2 and High Sierra and Rock
 * Ridge handling are disabled.
 */
odfs_err_t odfs_iso_mount_vd(odfs_cache_t *cache,
                             odfs_log_state_t *log,
                             uint32_t session_start,
                             uint32_t vd_lba,
                             int joliet,
                             odfs_node_t *root_out,
                             void **backend_ctx);

void odfs_iso_unmount(void *backend_ctx);

odfs_err_t odfs_iso_readdir(void *backend_ctx,
                            odfs_cache_t *cache,
                            odfs_log_state_t *log,
                            const odfs_node_t *dir,
                            odfs_dir_iter_fn callback,
                            void *cb_ctx,
                            uint32_t *resume_offset);

odfs_err_t odfs_iso_read(void *backend_ctx,
                         odfs_cache_t *cache,
                         odfs_log_state_t *log,
                         const odfs_node_t *file,
                         uint64_t offset,
                         void *buf,
                         size_t *len);

odfs_err_t odfs_iso_lookup(void *backend_ctx,
                           odfs_cache_t *cache,
                           odfs_log_state_t *log,
                           const odfs_node_t *dir,
                           const char *name,
                           odfs_node_t *out);

odfs_err_t odfs_iso_resolve_parent(void *backend_ctx,
                                   odfs_cache_t *cache,
                                   odfs_log_state_t *log,
                                   const odfs_node_t *dir,
                                   odfs_node_t *parent_out,
                                   odfs_node_t *grandparent_out);

odfs_err_t odfs_iso_get_volume_name(void *backend_ctx,
                                    char *buf, size_t buf_size);

uint32_t odfs_iso_get_volume_size(void *backend_ctx);

/* --- backend ops (exposed for registration) --- */

extern const odfs_backend_ops_t iso9660_backend_ops;

#endif /* ODFS_ISO9660_H */
