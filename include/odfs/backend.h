/*
 * odfs/backend.h — backend (format reader) interface
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef ODFS_BACKEND_H
#define ODFS_BACKEND_H

#include "odfs/error.h"
#include "odfs/node.h"
#include "odfs/cache.h"
#include "odfs/log.h"

/* forward declarations */
typedef struct odfs_mount odfs_mount_t;

/* directory enumeration callback */
typedef odfs_err_t (*odfs_dir_iter_fn)(const odfs_node_t *entry, void *ctx);

/* backend operations vtable */
typedef struct odfs_backend_ops {
    const char           *name;          /* e.g. "iso9660", "rock_ridge", "joliet" */
    odfs_backend_type_t  backend_type;  /* enum value for this backend */

    /*
     * Probe media for this format.
     * Returns ODFS_OK if format is detected, ODFS_ERR_BAD_FORMAT if not.
     */
    odfs_err_t (*probe)(odfs_cache_t *cache,
                         odfs_log_state_t *log,
                         uint32_t session_start);

    /*
     * Mount: parse root structures, populate root node.
     *   session_start — LBA offset for the session
     *   root_out      — populated with root directory node
     *   backend_ctx   — receives backend-private mount state (caller frees via unmount)
     */
    odfs_err_t (*mount)(odfs_cache_t *cache,
                         odfs_log_state_t *log,
                         uint32_t session_start,
                         odfs_node_t *root_out,
                         void **backend_ctx);

    /*
     * Unmount: release backend-private state.
     */
    void (*unmount)(void *backend_ctx);

    /*
     * List directory entries.
     *   resume_offset — if non-NULL, on input: byte offset to start from;
     *                   on output: byte offset of the next unvisited entry.
     *                   Pass NULL to iterate from the beginning.
     */
    odfs_err_t (*readdir)(void *backend_ctx,
                           odfs_cache_t *cache,
                           odfs_log_state_t *log,
                           const odfs_node_t *dir,
                           odfs_dir_iter_fn callback,
                           void *cb_ctx,
                           uint32_t *resume_offset);

    /*
     * Read file data.
     *   offset — byte offset into file
     *   buf    — output buffer
     *   len    — bytes to read (in), bytes actually read (out)
     */
    odfs_err_t (*read)(void *backend_ctx,
                        odfs_cache_t *cache,
                        odfs_log_state_t *log,
                        const odfs_node_t *file,
                        uint64_t offset,
                        void *buf,
                        size_t *len);

    /*
     * Lookup a child node by name within a directory.
     */
    odfs_err_t (*lookup)(void *backend_ctx,
                          odfs_cache_t *cache,
                          odfs_log_state_t *log,
                          const odfs_node_t *dir,
                          const char *name,
                          odfs_node_t *out);

    /*
     * Get volume name. May be NULL if not applicable.
     *   buf      — output buffer
     *   buf_size — size of output buffer
     */
    odfs_err_t (*get_volume_name)(void *backend_ctx,
                                   char *buf,
                                   size_t buf_size);
} odfs_backend_ops_t;

const char *odfs_backend_type_name(odfs_backend_type_t type);

#endif /* ODFS_BACKEND_H */
