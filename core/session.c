/*
 * session.c — multisession discovery
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Three strategies for finding the last session:
 *   1. explicit last-session query from device (Amiga SCSI)
 *   2. TOC from device (heuristic: last data track)
 *   3. PVD scan (host images): scan backwards for CD001 signatures
 */

#include "odfs/api.h"
#include "odfs/media.h"
#include "odfs/log.h"
#include "odfs/config.h"

#include <string.h>
#include <inttypes.h>

#define ISO_VD_START_OFFSET 16
#define ISO_SECTOR_SIZE     2048

/*
 * Find the last session start LBA.
 *
 * First tries the explicit media last-session query.
 * Falls back to the historical TOC heuristic, then to scanning
 * the image for PVD signatures.
 *
 *   media        — media handle
 *   log          — log state (may be NULL)
 *   last_lba_out — receives the start LBA of the last session
 *
 * Returns ODFS_OK if a session was found, error otherwise.
 * On failure, *last_lba_out is set to 0 (use first session).
 */
odfs_err_t odfs_find_last_session(odfs_media_t *media,
                                    odfs_log_state_t *log,
                                    uint32_t *last_lba_out)
{
    odfs_err_t err;

    (void)log;

    *last_lba_out = 0;

    /* strategy 1: explicit last-session query */
    err = odfs_media_read_last_session_lba(media, last_lba_out);
    if (err == ODFS_OK) {
        ODFS_INFO(log, ODFS_SUB_MULTISESSION,
                  "explicit last session starts at LBA %" PRIu32,
                  *last_lba_out);
        return ODFS_OK;
    }
    if (err != ODFS_ERR_UNSUPPORTED) {
        ODFS_INFO(log, ODFS_SUB_MULTISESSION,
                  "explicit last-session query unavailable "
                  "(%s); falling back to TOC heuristics",
                  odfs_err_str(err));
    }

    /* strategy 2: device TOC heuristic */
    {
        odfs_toc_t toc;
        memset(&toc, 0, sizeof(toc));
        err = odfs_media_read_toc(media, &toc);
        if (err == ODFS_OK && toc.session_count > 0) {
            int found_data = 0;
            uint8_t last = 0;

            for (uint8_t i = 0; i < toc.session_count; i++) {
                if ((toc.sessions[i].control & 0x04) != 0) {
                    last = i;
                    found_data = 1;
                }
            }

            if (!found_data) {
                ODFS_INFO(log, ODFS_SUB_MULTISESSION,
                          "TOC: %" PRIu32
                          " track(s), no data track found; using LBA 0",
                          (uint32_t)toc.session_count);
                return ODFS_OK;
            }

            *last_lba_out = toc.sessions[last].start_lba;
            ODFS_INFO(log, ODFS_SUB_MULTISESSION,
                       "TOC heuristic picked last data track "
                       "from %" PRIu32 " track(s) at LBA %" PRIu32,
                       (uint32_t)toc.session_count, *last_lba_out);
            return ODFS_OK;
        }
    }

    /* strategy 3: scan for PVD signatures (host images) */
    {
        uint32_t total = odfs_media_sector_count(media);
        uint32_t best_lba = 0;
        uint8_t buf[ISO_SECTOR_SIZE];

        if (total == 0)
            return ODFS_OK; /* unknown size, use LBA 0 */

        /*
         * Scan every potential PVD location. A session's PVD is at
         * session_start + 16. We look for CD001 at byte 1 and type
         * 0x01 at byte 0. Track the highest LBA where we find one.
         */
        for (uint32_t lba = ISO_VD_START_OFFSET; lba + ISO_VD_START_OFFSET < total; ) {
            err = odfs_media_read(media, lba, 1, buf);
            if (err != ODFS_OK)
                break;

            if (buf[0] == 0x01 &&
                memcmp(&buf[1], "CD001", 5) == 0) {
                uint32_t session_start = lba - ISO_VD_START_OFFSET;
                if (session_start > best_lba || best_lba == 0)
                    best_lba = session_start;

                /* read volume space size to jump past this session */
                uint32_t vol_size = (uint32_t)buf[80] |
                                   ((uint32_t)buf[81] << 8) |
                                   ((uint32_t)buf[82] << 16) |
                                   ((uint32_t)buf[83] << 24);
                if (vol_size > 0 && session_start + vol_size + ISO_VD_START_OFFSET < total) {
                    lba = session_start + vol_size + ISO_VD_START_OFFSET;
                    continue;
                }
            }

            /* no more sessions found from this point */
            break;
        }

        if (best_lba > 0) {
            *last_lba_out = best_lba;
            ODFS_INFO(log, ODFS_SUB_MULTISESSION,
                       "PVD scan found last session at LBA %" PRIu32,
                       best_lba);
        }
    }

    return ODFS_OK;
}
