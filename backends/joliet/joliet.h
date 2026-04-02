/*
 * joliet.h — Joliet (SVD with UCS-2 names) backend
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef ODFS_JOLIET_H
#define ODFS_JOLIET_H

#include "odfs/backend.h"
#include "iso9660/iso9660.h"

/* Joliet escape sequences in SVD (byte 88-90 of SVD) */
#define JOLIET_ESC_UCS2_LEVEL1  "%/@"
#define JOLIET_ESC_UCS2_LEVEL2  "%/C"
#define JOLIET_ESC_UCS2_LEVEL3  "%/E"

/* Joliet mount context */
typedef struct joliet_context {
    iso_pvd_info_t svd;            /* parsed from SVD (same structure as PVD) */
    uint32_t       session_start;
    uint32_t       next_node_id;
} joliet_context_t;

extern const odfs_backend_ops_t joliet_backend_ops;

#endif /* ODFS_JOLIET_H */
