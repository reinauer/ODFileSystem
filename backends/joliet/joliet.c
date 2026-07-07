/*
 * joliet.c — Joliet backend (SVD with UCS-2 filenames)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Joliet uses a Supplementary Volume Descriptor (type 2) with UCS-2
 * encoded filenames and its own directory tree, but the record layout
 * and all enumeration, read and parent-resolution logic are plain
 * ISO 9660. The shared machinery lives in the ISO backend; this file
 * only locates the SVD and mounts an ISO context in Joliet mode.
 */

#include "joliet.h"
#include "odfs/cache.h"
#include "odfs/error.h"
#include "odfs/log.h"

#include <string.h>
#include <inttypes.h>

/*
 * Scan the volume descriptor set for a Supplementary VD with Joliet
 * escape sequences in the escape field (bytes 88-90).
 */
static odfs_err_t joliet_find_svd(odfs_cache_t *cache,
                                  uint32_t session_start,
                                  uint32_t *lba_out)
{
    uint32_t lba;

    for (lba = session_start + ISO_VD_START_LBA; ; lba++) {
        const uint8_t *sector;
        odfs_err_t err = odfs_cache_read(cache, lba, &sector);
        if (err != ODFS_OK)
            return err;

        /* check CD001 signature */
        if (memcmp(&sector[1], ISO_STANDARD_ID, ISO_STANDARD_ID_LEN) != 0)
            return ODFS_ERR_BAD_FORMAT;

        if (sector[0] == ISO_VD_TYPE_TERM)
            break; /* no Joliet SVD found */

        if (sector[0] == ISO_VD_TYPE_SUPPL) {
            const uint8_t *esc = &sector[88];
            if (esc[0] == '%' && esc[1] == '/' &&
                (esc[2] == '@' || esc[2] == 'C' || esc[2] == 'E')) {
                *lba_out = lba;
                return ODFS_OK;
            }
        }

        /* safety: don't scan forever */
        if (lba > session_start + ISO_VD_START_LBA + 32)
            break;
    }

    return ODFS_ERR_BAD_FORMAT;
}

static odfs_err_t joliet_probe(odfs_cache_t *cache,
                                odfs_log_state_t *log,
                                uint32_t session_start)
{
    uint32_t svd_lba;
    odfs_err_t err = joliet_find_svd(cache, session_start, &svd_lba);

    if (err == ODFS_OK)
        ODFS_INFO(log, ODFS_SUB_JOLIET,
                   "Joliet SVD found at LBA %" PRIu32, svd_lba);
    else
        ODFS_LOG(log, ODFS_LOG_DEBUG, ODFS_SUB_JOLIET, "no Joliet SVD");
    return err;
}

static odfs_err_t joliet_mount(odfs_cache_t *cache,
                                odfs_log_state_t *log,
                                uint32_t session_start,
                                odfs_node_t *root_out,
                                void **backend_ctx)
{
    uint32_t svd_lba;
    odfs_err_t err = joliet_find_svd(cache, session_start, &svd_lba);

    if (err != ODFS_OK)
        return err;

    return odfs_iso_mount_vd(cache, log, session_start, svd_lba, 1,
                             root_out, backend_ctx);
}

/* ------------------------------------------------------------------ */
/* backend ops table                                                   */
/* ------------------------------------------------------------------ */

const odfs_backend_ops_t joliet_backend_ops = {
    .name            = "joliet",
    .backend_type    = ODFS_BACKEND_JOLIET,
    .probe           = joliet_probe,
    .mount           = joliet_mount,
    .unmount         = odfs_iso_unmount,
    .readdir         = odfs_iso_readdir,
    .read            = odfs_iso_read,
    .lookup          = odfs_iso_lookup,
    .resolve_parent  = odfs_iso_resolve_parent,
    .get_volume_name = odfs_iso_get_volume_name,
    .get_volume_size = odfs_iso_get_volume_size,
};
