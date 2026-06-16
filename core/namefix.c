/*
 * core/namefix.c — deterministic directory name collision handling
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "odfs/namefix.h"

#include "odfs/alloc.h"
#include "odfs/node.h"
#include "odfs/printf.h"
#include "odfs/string.h"

#include <string.h>

#define ODFS_NAMEFIX_CHUNK_SIZE 4096u

void odfs_namefix_init(odfs_namefix_state_t *state)
{
    state->head = NULL;
    state->chunks = NULL;
}

void odfs_namefix_destroy(odfs_namefix_state_t *state)
{
    odfs_namefix_chunk_t *chunk = state->chunks;

    while (chunk) {
        odfs_namefix_chunk_t *next = chunk->next;
        odfs_free(chunk);
        chunk = next;
    }

    state->head = NULL;
    state->chunks = NULL;
}

static size_t odfs_namefix_align(size_t size)
{
    size_t align = sizeof(void *);

    return (size + align - 1u) & ~(align - 1u);
}

static void *odfs_namefix_alloc(odfs_namefix_state_t *state, size_t size)
{
    odfs_namefix_chunk_t *chunk;
    size_t chunk_size = ODFS_NAMEFIX_CHUNK_SIZE;
    size_t total;

    if (!state || size == 0)
        return NULL;

    size = odfs_namefix_align(size);
    chunk = state->chunks;
    if (chunk && size <= chunk->size - chunk->used) {
        void *ptr = (unsigned char *)(chunk + 1) + chunk->used;
        chunk->used += size;
        return ptr;
    }

    if (size > chunk_size)
        chunk_size = size;
    if (chunk_size > (size_t)-1 - sizeof(*chunk))
        return NULL;

    total = sizeof(*chunk) + chunk_size;
    chunk = odfs_malloc(total);
    if (!chunk)
        return NULL;

    chunk->next = state->chunks;
    chunk->used = size;
    chunk->size = chunk_size;
    state->chunks = chunk;
    return (void *)(chunk + 1);
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
    size_t len = strlen(name) + 1u;
    odfs_namefix_entry_t *entry;

    entry = odfs_namefix_alloc(state, sizeof(*entry) + len);
    if (!entry)
        return ODFS_ERR_NOMEM;

    memcpy(entry->name, name, len);
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
        int suffix_len = odfs_snprintf(suffix, sizeof(suffix), "~%u", ordinal);
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
