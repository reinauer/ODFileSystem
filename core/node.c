/*
 * node.c — node model helpers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "odfs/node.h"

static const char *backend_names[] = {
    [ODFS_BACKEND_NONE]       = "none",
    [ODFS_BACKEND_ISO9660]    = "iso9660",
    [ODFS_BACKEND_ROCK_RIDGE] = "rock_ridge",
    [ODFS_BACKEND_JOLIET]     = "joliet",
    [ODFS_BACKEND_UDF]        = "udf",
    [ODFS_BACKEND_HFS]        = "hfs",
    [ODFS_BACKEND_HFSPLUS]    = "hfsplus",
    [ODFS_BACKEND_CDDA]       = "cdda",
};

static const char *kind_names[] = {
    [ODFS_NODE_FILE]    = "file",
    [ODFS_NODE_DIR]     = "dir",
    [ODFS_NODE_SYMLINK] = "symlink",
    [ODFS_NODE_VIRTUAL] = "virtual",
};

/* note: also declared in backend.h, single definition here */
const char *odfs_backend_type_name(odfs_backend_type_t type)
{
    if (type >= 0 && type < ODFS_BACKEND__COUNT)
        return backend_names[type];
    return "unknown";
}

const char *odfs_node_kind_name(odfs_node_kind_t kind)
{
    if (kind >= 0 && kind < ODFS_NODE__COUNT)
        return kind_names[kind];
    return "unknown";
}
