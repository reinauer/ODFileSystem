/*
 * ancestry.h — parent/ancestor lookup helpers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef ODFS_ANCESTRY_H
#define ODFS_ANCESTRY_H

#include "odfs/error.h"
#include "odfs/node.h"

typedef struct odfs_mount odfs_mount_t;

/*
 * Resolve the parent of a node by walking the mounted directory tree.
 *
 * parent_out receives the containing directory. grandparent_out receives the
 * parent of that directory, or the mount root when parent_out is the root.
 */
odfs_err_t odfs_resolve_parent_node(odfs_mount_t *mnt,
                                    const odfs_node_t *node,
                                    odfs_node_t *parent_out,
                                    odfs_node_t *grandparent_out);

#endif /* ODFS_ANCESTRY_H */
