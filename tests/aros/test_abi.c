/*
 * Exercise the production packet helpers with the real AROS SDK layout.
 * This isolated x86_64 harness runs on Linux; it makes no OS calls.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "../../platform/amiga/handler_main.c"

_Static_assert(ODFS_PF_SECTORS == 16, "AROS streaming window disabled");

static UBYTE dma_test_mem[(32 + ODFS_PF_SECTORS) * 2048 + 32];
static ULONG dma_test_limit;
static ULONG dma_test_calls;
static int dma_test_public_only;

void *odfs_amiga_alloc_mem(ULONG size, ULONG flags)
{
    dma_test_calls++;
    if (size > dma_test_limit ||
        (dma_test_public_only && flags != MEMF_PUBLIC))
        return NULL;
    /* Exercise the buffer's alignment padding, too. */
    return dma_test_mem + 1;
}

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

    {
        handler_global_t g;

        memset(&g, 0, sizeof(g));
        g.sector_size = 2048;
        dma_test_limit = sizeof(dma_test_mem);
        if (!alloc_dma_buffer(&g, MEMF_PUBLIC | MEMF_CHIP) ||
            g.pf_capacity != ODFS_PF_SECTORS || dma_test_calls != 1 ||
            ((uintptr_t)g.dma_buf & 15) || g.dma_buf_size != 65536 ||
            g.pf_buf != g.dma_buf + g.dma_buf_size ||
            g.pf_buf + g.pf_capacity * 2048 >
                g.dma_buf_raw + g.dma_buf_raw_size)
            return 19;

        memset(&g, 0, sizeof(g));
        g.sector_size = 2048;
        dma_test_calls = 0;
        dma_test_public_only = 1;
        if (!alloc_dma_buffer(&g, MEMF_PUBLIC | MEMF_CHIP) ||
            dma_test_calls != 2 || g.pf_capacity != ODFS_PF_SECTORS)
            return 20;

        memset(&g, 0, sizeof(g));
        g.sector_size = 2048;
        dma_test_calls = 0;
        dma_test_public_only = 0;
        dma_test_limit = 34 * 2048 + 15;
        if (!alloc_dma_buffer(&g, MEMF_PUBLIC | MEMF_CHIP) ||
            g.pf_capacity != 2 || dma_test_calls != 3 ||
            g.dma_buf_raw_size != dma_test_limit ||
            g.pf_buf + 4096 > g.dma_buf_raw + g.dma_buf_raw_size)
            return 21;

        memset(&g, 0, sizeof(g));
        g.sector_size = 2048;
        dma_test_calls = 0;
        dma_test_limit = 0;
        if (alloc_dma_buffer(&g, MEMF_PUBLIC | MEMF_CHIP) ||
            dma_test_calls != 4 || g.dma_buf_raw || g.pf_buf)
            return 22;
    }

    /* Streaming, random and short-file requests obey MaxTransfer too. */
    {
        handler_global_t g;

        memset(&g, 0, sizeof(g));
        g.pf_capacity = ODFS_PF_SECTORS;
        g.max_transfer_blocks = 64;
        if (prefetch_sector_count(&g, 100, 1) != 16 ||
            prefetch_sector_count(&g, 100, 0) != 2 ||
            prefetch_sector_count(&g, 1, 1) != 1)
            return 23;
        g.max_transfer_blocks = 3;
        if (prefetch_sector_count(&g, 100, 1) != 3)
            return 24;
        g.max_transfer_blocks = 1;
        if (prefetch_sector_count(&g, 100, 1) != 1 ||
            prefetch_sector_count(&g, 100, 0) != 1)
            return 25;
        g.max_transfer_blocks = 64;
        g.pf_capacity = 2;
        if (prefetch_sector_count(&g, 100, 1) != 2)
            return 26;
    }

    /* READ(10) LBAs must remain big-endian on little-endian AROS. */
    {
        ULONG cdb[3] = { 0, 0, 0 };
        static const UBYTE expected[4] = { 0x12, 0x34, 0x56, 0x78 };

        cdb[1] = scsi_lba_be(UINT32_C(0x12345678));
        if (memcmp((UBYTE *)cdb + 4, expected, sizeof(expected)))
            return 17;
        cdb[1] = scsi_lba_be(16);
        if (((UBYTE *)cdb)[4] || ((UBYTE *)cdb)[5] ||
            ((UBYTE *)cdb)[6] || ((UBYTE *)cdb)[7] != 16)
            return 18;
    }

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
        struct InfoData disk_info;
        int64_t oldpos;

        memset(&g, 0, sizeof(g));
        memset(&entry, 0, sizeof(entry));
        memset(&fh, 0, sizeof(fh));
        g.mounted = 1;
        if (odfs_handler_fill_info(&g, NULL, &disk_info) ||
            disk_info.id_DiskType != ID_CDFS_DISK)
            return 16;
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
