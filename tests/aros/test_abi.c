/*
 * Exercise the production packet helpers with the real AROS SDK layout.
 * This isolated x86_64 harness runs on Linux; it makes no OS calls.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "../../platform/amiga/handler_main.c"

int main(void)
{
    union {
        struct ExAllData align;
        UBYTE bytes[1024];
    } buffer;
    odfs_node_t node;
    odfs_lock_t lock;
    char name[16];
    UBYTE bstrbuf[16] __attribute__((aligned(8)));
    BSTR bstr = (BSTR)MKBADDR(bstrbuf);
    struct FileInfoBlock fib;

    /* On x86_64 the stack addresses also catch pointer truncation. */
    memset(&lock, 0, sizeof(lock));
    if (LOCK_FROM_BPTR((SIPTR)LOCK_TO_BPTR(&lock)) != &lock)
        return 1;
    AROS_BSTR_setstrlen(bstr, 5);
    memcpy(AROS_BSTR_ADDR(bstr), "hello", 5);
    bstr_to_cstr(bstr, name, sizeof(name));
    if (strcmp(name, "hello") || odfs_bstr_len(bstr) != 5)
        return 2;
    bstr_to_cstr(bstr, name, 4);
    if (strcmp(name, "hel"))
        return 3;
    bstr_to_cstr(0, name, sizeof(name));
    if (name[0])
        return 4;

    memset(&node, 0, sizeof(node));
    node.kind = ODFS_NODE_FILE;
    node.backend = ODFS_BACKEND_ISO9660;
    node.extent.lba = 42;
    node.size = 12345;
    strcpy(node.name, "filename.txt");
    node.amiga_as.has_comment = 1;
    strcpy(node.amiga_as.comment, "comment");

    fill_fib(NULL, &fib, &node);
    if (fib.fib_FileName[0] != strlen(node.name) ||
        memcmp(fib.fib_FileName + 1, node.name, strlen(node.name)) ||
        fib.fib_Size != 12345 || fib.fib_Comment[0] != 7)
        return 5;

    for (LONG data = ED_NAME; data <= ED_OWNER; data++) {
        struct ExAllData *ed = (struct ExAllData *)buffer.bytes;
        struct ExAllData *cursor = ed;
        LONG remaining = sizeof(buffer.bytes);
        size_t used;

        memset(&buffer, 0xa5, sizeof(buffer));
        if (!exall_fill_entry(NULL, &cursor, &remaining, data, &node))
            return 6;
        used = (size_t)((UBYTE *)cursor - buffer.bytes);
        if (strcmp((char *)ed->ed_Name, node.name) ||
            used % __alignof__(struct ExAllData) != 0 ||
            remaining != (LONG)(sizeof(buffer.bytes) - used))
            return 7;
        if (data >= ED_TYPE && ed->ed_Type != ST_FILE)
            return 8;
        if (data >= ED_SIZE && ed->ed_Size != 12345)
            return 9;
        if (data >= ED_COMMENT &&
            strcmp((char *)ed->ed_Comment, "comment"))
            return 10;
        /* The next entry must be aligned and leave the first intact. */
        if (!exall_fill_entry(NULL, &cursor, &remaining, data, &node) ||
            strcmp((char *)ed->ed_Name, node.name))
            return 11;
        /* A one-byte-short buffer must remain untouched. */
        cursor = ed;
        remaining = (LONG)used - 1;
        memset(&buffer, 0xa5, sizeof(buffer));
        if (exall_fill_entry(NULL, &cursor, &remaining, data, &node) ||
            buffer.bytes[0] != 0xa5 || cursor != ed)
            return 12;
    }

    {
        handler_global_t g;
        odfs_entry_t entry;
        odfs_fh_t fh;
        int64_t oldpos;

        memset(&g, 0, sizeof(g));
        memset(&entry, 0, sizeof(entry));
        memset(&fh, 0, sizeof(fh));
        g.mounted = 1;
        entry.fnode.size = UINT64_C(0x200000000);
        fh.entry = &entry;
        fh.magic = ODFS_FH_MAGIC;
        if (odfs_handler_seek_object(&g, &fh, INT64_C(0x100000000),
                                     OFFSET_BEGINNING, &oldpos) ||
            fh.pos != UINT64_C(0x100000000) || oldpos != 0)
            return 13;
        if (odfs_handler_seek_object(&g, &fh, INT64_MAX, OFFSET_CURRENT,
                                     &oldpos) != ERROR_SEEK_ERROR ||
            fh.pos != UINT64_C(0x100000000))
            return 14;
        if (odfs_handler_seek_object(&g, &fh, INT64_MIN, OFFSET_END,
                                     &oldpos) != ERROR_SEEK_ERROR)
            return 15;
    }
    return 0;
}
