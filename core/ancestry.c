/*
 * ancestry.c — parent/ancestor lookup helpers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "odfs/ancestry.h"

#if ODFS_PARENT_SEARCH

#include "odfs/api.h"
#include "odfs/alloc.h"

#include <string.h>

/*
 * Deepest directory level the search descends to. Plain ISO 9660 stops
 * at 8 levels, but Rock Ridge relocation, UDF and HFS allow deeper
 * trees; the cap only has to keep a malformed tree from growing the
 * frame array without bound.
 */
#define ANCESTRY_MAX_DEPTH 256

typedef struct ancestry_frame {
    odfs_node_t dir;
    uint32_t    resume;
} ancestry_frame_t;

/*
 * Directories already entered, keyed by extent, so that a directory
 * listed in several places, or in a cycle, is read only once. Open
 * addressing; a zero extent marks a free slot, so virtual directories
 * without an extent are not tracked and rely on the depth cap.
 */
typedef struct ancestry_seen {
    odfs_extent_t *slot;
    size_t         mask;
    size_t         count;
} ancestry_seen_t;

static int ancestry_seen_insert(odfs_extent_t *slot, size_t mask,
                                const odfs_extent_t *e)
{
    size_t i = (size_t)(e->lba ^ (e->lba >> 11) ^ e->length) & mask;

    while (slot[i].lba != 0 || slot[i].length != 0) {
        if (slot[i].lba == e->lba && slot[i].length == e->length)
            return 0;
        i = (i + 1) & mask;
    }
    slot[i] = *e;
    return 1;
}

/* 1 if e is new, 0 if it was entered before, -1 if out of memory */
static int ancestry_seen_add(ancestry_seen_t *seen, const odfs_extent_t *e)
{
    if (e->lba == 0 && e->length == 0)
        return 1;

    if (2 * (seen->count + 1) > seen->mask + 1) {
        size_t mask = 2 * seen->mask + 1;
        odfs_extent_t *slot = odfs_calloc(mask + 1, sizeof(*slot));
        size_t i;

        if (!slot)
            return -1;
        for (i = 0; i <= seen->mask; i++) {
            if (seen->slot[i].lba != 0 || seen->slot[i].length != 0)
                ancestry_seen_insert(slot, mask, &seen->slot[i]);
        }
        odfs_free(seen->slot);
        seen->slot = slot;
        seen->mask = mask;
    }

    if (!ancestry_seen_insert(seen->slot, seen->mask, e))
        return 0;
    seen->count++;
    return 1;
}

typedef struct ancestry_search_ctx {
    const odfs_node_t *target;
    ancestry_seen_t    seen;
    ancestry_frame_t  *child_frame;
    size_t             depth;
    int                found;
    int                descend;
    int                truncated;
} ancestry_search_ctx_t;

static odfs_err_t ancestry_search_cb(const odfs_node_t *entry, void *ctx)
{
    ancestry_search_ctx_t *asc = ctx;

    if (odfs_node_matches_identity(entry, asc->target)) {
        asc->found = 1;
        return ODFS_ERR_EOF;
    }

    if (!asc->descend && entry->kind == ODFS_NODE_DIR) {
        int added;

        if (asc->depth >= ANCESTRY_MAX_DEPTH) {
            asc->truncated = 1;
            return ODFS_OK;
        }
        added = ancestry_seen_add(&asc->seen, &entry->extent);
        if (added < 0)
            return ODFS_ERR_NOMEM;
        if (added == 0)
            return ODFS_OK; /* searched already, or being searched */
        asc->child_frame->dir = *entry;
        asc->child_frame->resume = 0;
        asc->descend = 1;
        return ODFS_ERR_EOF;
    }

    return ODFS_OK;
}

/*
 * Generic fallback: locate a node's parent by depth-first search of the
 * mounted tree. O(directories on the volume); used only when the backend
 * has no resolve_parent op (or it declined the request).
 */
odfs_err_t odfs_resolve_parent_search(odfs_mount_t *mnt,
                                      const odfs_node_t *node,
                                      odfs_node_t *parent_out,
                                      odfs_node_t *grandparent_out)
{
    ancestry_frame_t *frames;
    ancestry_search_ctx_t asc;
    size_t cap = 8;
    size_t depth = 1;
    odfs_err_t err = ODFS_ERR_NOT_FOUND;

    frames = odfs_malloc(cap * sizeof(*frames));
    asc.seen.slot = odfs_calloc(64, sizeof(*asc.seen.slot));
    asc.seen.mask = 63;
    asc.seen.count = 0;
    if (!frames || !asc.seen.slot) {
        odfs_free(asc.seen.slot);
        odfs_free(frames);
        return ODFS_ERR_NOMEM;
    }

    frames[0].dir = mnt->root;
    frames[0].resume = 0;
    ancestry_seen_add(&asc.seen, &mnt->root.extent);

    asc.truncated = 0;

    while (depth > 0) {
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
        asc.depth = depth;
        asc.found = 0;
        asc.descend = 0;

        err = odfs_readdir(mnt, &frames[depth - 1].dir, ancestry_search_cb,
                           &asc, &frames[depth - 1].resume);
        if (asc.found) {
            *parent_out = frames[depth - 1].dir;
            if (grandparent_out) {
                if (depth > 1)
                    *grandparent_out = frames[depth - 2].dir;
                else
                    *grandparent_out = mnt->root;
            }
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

    /* every directory was read to its end without a match; if some
     * were too deep to enter, the tree is malformed or looped */
    if (depth == 0)
        err = asc.truncated ? ODFS_ERR_LOOP : ODFS_ERR_NOT_FOUND;

    odfs_free(asc.seen.slot);
    odfs_free(frames);
    return err;
}

#endif /* ODFS_PARENT_SEARCH */
