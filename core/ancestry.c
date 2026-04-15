/*
 * ancestry.c — parent/ancestor lookup helpers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "odfs/ancestry.h"

#include "odfs/api.h"
#include "odfs/alloc.h"

#include <string.h>

typedef struct ancestry_frame {
    odfs_node_t dir;
    uint32_t    resume;
} ancestry_frame_t;

typedef struct ancestry_search_ctx {
    const odfs_node_t *target;
    ancestry_frame_t  *child_frame;
    int                found;
    int                descend;
} ancestry_search_ctx_t;

static int ancestry_is_root(const odfs_mount_t *mnt, const odfs_node_t *node)
{
    if (!mnt || !node)
        return 0;

    return node->kind == mnt->root.kind &&
           node->backend == mnt->root.backend &&
           node->id == mnt->root.id &&
           node->extent.lba == mnt->root.extent.lba &&
           node->extent.length == mnt->root.extent.length;
}

static int ancestry_matches_identity(const odfs_node_t *a, const odfs_node_t *b)
{
    if (!a || !b)
        return 0;

    return a->backend == b->backend &&
           a->kind == b->kind &&
           a->size == b->size &&
           a->extent.lba == b->extent.lba &&
           a->extent.length == b->extent.length &&
           strcmp(a->name, b->name) == 0;
}

static odfs_err_t ancestry_search_cb(const odfs_node_t *entry, void *ctx)
{
    ancestry_search_ctx_t *asc = ctx;

    if (ancestry_matches_identity(entry, asc->target)) {
        asc->found = 1;
        return ODFS_ERR_EOF;
    }

    if (!asc->descend && entry->kind == ODFS_NODE_DIR) {
        asc->child_frame->dir = *entry;
        asc->child_frame->resume = 0;
        asc->descend = 1;
        return ODFS_ERR_EOF;
    }

    return ODFS_OK;
}

odfs_err_t odfs_resolve_parent_node(odfs_mount_t *mnt,
                                    const odfs_node_t *node,
                                    odfs_node_t *parent_out,
                                    odfs_node_t *grandparent_out)
{
    ancestry_frame_t *frames;
    size_t cap = 8;
    size_t depth = 1;
    odfs_err_t err = ODFS_ERR_NOT_FOUND;

    if (!mnt || !node || !parent_out || !grandparent_out)
        return ODFS_ERR_INVAL;

    if (ancestry_is_root(mnt, node))
        return ODFS_ERR_NOT_FOUND;

    frames = odfs_malloc(cap * sizeof(*frames));
    if (!frames)
        return ODFS_ERR_NOMEM;

    frames[0].dir = mnt->root;
    frames[0].resume = 0;

    while (depth > 0) {
        ancestry_search_ctx_t asc;

        if (depth == cap) {
            ancestry_frame_t *grown;
            size_t new_cap = cap * 2;

            grown = odfs_malloc(new_cap * sizeof(*grown));
            if (!grown) {
                err = ODFS_ERR_NOMEM;
                break;
            }
            memcpy(grown, frames, cap * sizeof(*grown));
            odfs_free(frames);
            frames = grown;
            cap = new_cap;
        }

        asc.target = node;
        asc.child_frame = &frames[depth];
        asc.found = 0;
        asc.descend = 0;

        err = odfs_readdir(mnt, &frames[depth - 1].dir, ancestry_search_cb,
                           &asc, &frames[depth - 1].resume);
        if (asc.found) {
            *parent_out = frames[depth - 1].dir;
            if (depth > 1)
                *grandparent_out = frames[depth - 2].dir;
            else
                *grandparent_out = mnt->root;
            err = ODFS_OK;
            break;
        }

        if (asc.descend) {
            depth++;
            continue;
        }

        if (err != ODFS_OK) {
            if (err == ODFS_ERR_EOF)
                err = ODFS_ERR_NOT_FOUND;
            break;
        }

        depth--;
    }

    odfs_free(frames);
    return err;
}
