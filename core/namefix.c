/*
 * core/namefix.c — deterministic directory name collision handling
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "odfs/namefix.h"

#include "odfs/alloc.h"
#include "odfs/node.h"
#include "odfs/string.h"

#include <stddef.h>
#include <string.h>

#define ODFS_NAMEFIX_CHUNK_SIZE 4096u

void odfs_namefix_init(odfs_namefix_state_t *state)
{
    memset(state->buckets, 0, sizeof(state->buckets));
    state->chunks = NULL;
    state->free = (unsigned char *)state->inline_buf;
    state->avail = sizeof(state->inline_buf);
}

void odfs_namefix_destroy(odfs_namefix_state_t *state)
{
    odfs_namefix_chunk_t *chunk = state->chunks;

    while (chunk) {
        odfs_namefix_chunk_t *next = chunk->next;
        odfs_free(chunk);
        chunk = next;
    }

    odfs_namefix_init(state);
}

static void *odfs_namefix_alloc(odfs_namefix_state_t *state, size_t size)
{
    odfs_namefix_chunk_t *chunk;
    size_t chunk_size = ODFS_NAMEFIX_CHUNK_SIZE;
    void *ptr;

    size = (size + sizeof(void *) - 1u) & ~(sizeof(void *) - 1u);
    if (size > state->avail) {
        if (size > chunk_size)
            chunk_size = size;
        chunk = odfs_malloc(sizeof(*chunk) + chunk_size);
        if (!chunk)
            return NULL;
        chunk->next = state->chunks;
        state->chunks = chunk;
        state->free = (unsigned char *)(chunk + 1);
        state->avail = chunk_size;
    }

    ptr = state->free;
    state->free += size;
    state->avail -= size;
    return ptr;
}

/* odfs_strcasehash() of name, also returning its length */
static unsigned int odfs_namefix_hash(const char *name, size_t *len_out)
{
    const char *s = name;
    unsigned int hash = 0;
    unsigned int c;

    while ((c = (unsigned char)*s++) != 0) {
        hash = (hash << 5) | (hash >> 27);
        hash ^= c | 0x20u;
    }
    hash ^= hash >> 16;
    hash ^= hash >> 8;
    *len_out = (size_t)(s - name - 1);
    return hash;
}

static int odfs_namefix_contains(const odfs_namefix_state_t *state,
                                 const char *name, unsigned int hash)
{
    const odfs_namefix_entry_t *entry =
        state->buckets[hash % ODFS_NAMEFIX_BUCKETS];

    while (entry) {
        if (entry->hash == hash && odfs_strcasecmp(entry->name, name) == 0)
            return 1;
        entry = entry->hash_next;
    }
    return 0;
}

static odfs_err_t odfs_namefix_remember(odfs_namefix_state_t *state,
                                        const char *name, size_t len,
                                        unsigned int hash)
{
    odfs_namefix_entry_t *entry;
    unsigned int bucket = hash % ODFS_NAMEFIX_BUCKETS;

    entry = odfs_namefix_alloc(state,
                               offsetof(odfs_namefix_entry_t, name) +
                               len + 1u);
    if (!entry)
        return ODFS_ERR_NOMEM;

    memcpy(entry->name, name, len + 1u);
    entry->hash = hash;
    entry->hash_next = state->buckets[bucket];
    state->buckets[bucket] = entry;
    return ODFS_OK;
}

odfs_err_t odfs_namefix_apply(odfs_namefix_state_t *state,
                              char *name, size_t name_size)
{
    char base[ODFS_NAME_MAX];
    size_t len;
    size_t base_len;
    unsigned int hash;

    if (name_size == 0)
        return ODFS_ERR_INVAL;

    hash = odfs_namefix_hash(name, &len);
    if (!odfs_namefix_contains(state, name, hash))
        return odfs_namefix_remember(state, name, len, hash);

    memcpy(base, name, name_size);
    base[name_size - 1] = '\0';
    base_len = strlen(base);

    for (unsigned int ordinal = 2; ordinal < 1000000; ordinal++) {
        /* build "~<ordinal>" by hand: this is the only formatter use in
         * the ROM link, and going through odfs_snprintf() would keep
         * the whole printf_local formatter resident for one suffix */
        char suffix[16];
        char digits[10];
        unsigned int v = ordinal;
        size_t nd = 0;
        size_t suffix_len = 0;
        size_t keep = base_len;

        do {
            digits[nd++] = (char)('0' + v % 10u);
            v /= 10u;
        } while (v != 0 && nd < sizeof(digits));
        suffix[suffix_len++] = '~';
        while (nd > 0)
            suffix[suffix_len++] = digits[--nd];
        suffix[suffix_len] = '\0';

        if (suffix_len >= name_size)
            return ODFS_ERR_NAME_TOO_LONG;

        if (keep + (size_t)suffix_len >= name_size)
            keep = name_size - (size_t)suffix_len - 1;

        memcpy(name, base, keep);
        memcpy(name + keep, suffix, (size_t)suffix_len + 1);

        hash = odfs_namefix_hash(name, &len);
        if (!odfs_namefix_contains(state, name, hash))
            return odfs_namefix_remember(state, name, len, hash);
    }

    return ODFS_ERR_OVERFLOW;
}
