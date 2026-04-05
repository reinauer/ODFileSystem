/*
 * core/namefix.c — deterministic directory name collision handling
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "odfs/namefix.h"

#include "odfs/alloc.h"
#include "odfs/node.h"
#include "odfs/string.h"

#include <stdio.h>
#include <string.h>

void odfs_namefix_init(odfs_namefix_state_t *state)
{
    state->head = NULL;
}

void odfs_namefix_destroy(odfs_namefix_state_t *state)
{
    odfs_namefix_entry_t *entry = state->head;
    while (entry) {
        odfs_namefix_entry_t *next = entry->next;
        odfs_free(entry->name);
        odfs_free(entry);
        entry = next;
    }
    state->head = NULL;
}

static int odfs_namefix_contains(const odfs_namefix_state_t *state,
                                 const char *name)
{
    const odfs_namefix_entry_t *entry = state->head;
    while (entry) {
        if (odfs_strcasecmp(entry->name, name) == 0)
            return 1;
        entry = entry->next;
    }
    return 0;
}

static odfs_err_t odfs_namefix_remember(odfs_namefix_state_t *state,
                                        const char *name)
{
    size_t len = strlen(name);
    odfs_namefix_entry_t *entry = odfs_malloc(sizeof(*entry));
    char *copy;

    if (!entry)
        return ODFS_ERR_NOMEM;

    copy = odfs_malloc(len + 1);
    if (!copy) {
        odfs_free(entry);
        return ODFS_ERR_NOMEM;
    }

    memcpy(copy, name, len + 1);
    entry->name = copy;
    entry->next = state->head;
    state->head = entry;
    return ODFS_OK;
}

odfs_err_t odfs_namefix_apply(odfs_namefix_state_t *state,
                              char *name, size_t name_size)
{
    char base[ODFS_NAME_MAX];

    if (name_size == 0)
        return ODFS_ERR_INVAL;

    if (!odfs_namefix_contains(state, name))
        return odfs_namefix_remember(state, name);

    memcpy(base, name, name_size);
    base[name_size - 1] = '\0';

    for (unsigned int ordinal = 2; ordinal < 1000000; ordinal++) {
        char suffix[32];
        int suffix_len = snprintf(suffix, sizeof(suffix), "~%u", ordinal);
        size_t base_len = strlen(base);

        if (suffix_len < 0)
            return ODFS_ERR_INVAL;

        if ((size_t)suffix_len >= name_size)
            return ODFS_ERR_NAME_TOO_LONG;

        if (base_len + (size_t)suffix_len >= name_size)
            base_len = name_size - (size_t)suffix_len - 1;

        memcpy(name, base, base_len);
        memcpy(name + base_len, suffix, (size_t)suffix_len + 1);

        if (!odfs_namefix_contains(state, name))
            return odfs_namefix_remember(state, name);
    }

    return ODFS_ERR_OVERFLOW;
}
