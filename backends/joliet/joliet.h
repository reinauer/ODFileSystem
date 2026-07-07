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

/*
 * A Joliet mount has no context struct of its own: it shares the ISO
 * backend's iso_context_t (with ctx->joliet set) and every op except
 * probe and mount.
 */

extern const odfs_backend_ops_t joliet_backend_ops;

#endif /* ODFS_JOLIET_H */
