/*
 * odfs/namefix.h — deterministic directory name collision handling
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef ODFS_NAMEFIX_H
#define ODFS_NAMEFIX_H

#include "odfs/error.h"
#include <stddef.h>

#define ODFS_NAMEFIX_BUCKETS 64u

typedef struct odfs_namefix_entry {
    struct odfs_namefix_entry *next;
    struct odfs_namefix_entry *hash_next;
    char                       name[1];
} odfs_namefix_entry_t;

typedef struct odfs_namefix_chunk {
    struct odfs_namefix_chunk *next;
    size_t                     used;
    size_t                     size;
} odfs_namefix_chunk_t;

typedef struct odfs_namefix_state {
    odfs_namefix_entry_t *head;
    odfs_namefix_entry_t *buckets[ODFS_NAMEFIX_BUCKETS];
    odfs_namefix_chunk_t *chunks;
} odfs_namefix_state_t;

void odfs_namefix_init(odfs_namefix_state_t *state);
void odfs_namefix_destroy(odfs_namefix_state_t *state);
odfs_err_t odfs_namefix_apply(odfs_namefix_state_t *state,
                              char *name, size_t name_size);

#endif /* ODFS_NAMEFIX_H */
