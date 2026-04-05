/*
 * odfs/namefix.h — deterministic directory name collision handling
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef ODFS_NAMEFIX_H
#define ODFS_NAMEFIX_H

#include "odfs/error.h"
#include <stddef.h>

typedef struct odfs_namefix_entry {
    struct odfs_namefix_entry *next;
    char                      *name;
} odfs_namefix_entry_t;

typedef struct odfs_namefix_state {
    odfs_namefix_entry_t *head;
} odfs_namefix_state_t;

void odfs_namefix_init(odfs_namefix_state_t *state);
void odfs_namefix_destroy(odfs_namefix_state_t *state);
odfs_err_t odfs_namefix_apply(odfs_namefix_state_t *state,
                              char *name, size_t name_size);

#endif /* ODFS_NAMEFIX_H */
