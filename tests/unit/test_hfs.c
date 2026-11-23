/*
 * test_hfs.c — unit tests for the HFS and HFS+ backends
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "odfs/api.h"
#include "odfs/alloc.h"
#include "odfs/cache.h"
#include "hfs/hfs.h"
#include "hfsplus/hfsplus.h"
#include "test_harness.h"

#include <string.h>

#define IMG_SECTORS 160u
#define SECTOR_SIZE 2048u
#define IMG_BYTES   (IMG_SECTORS * SECTOR_SIZE)

static odfs_err_t mem_read_sectors(void *ctx, uint32_t lba,
                                   uint32_t count, void *buf)
{
    const uint8_t *img = ctx;

    if (lba > IMG_SECTORS || count > IMG_SECTORS - lba)
        return ODFS_ERR_RANGE;
    memcpy(buf, img + (size_t)lba * SECTOR_SIZE, (size_t)count * SECTOR_SIZE);
    return ODFS_OK;
}

static uint32_t mem_sector_size(void *ctx)
{
    (void)ctx;
    return SECTOR_SIZE;
}

static uint32_t mem_sector_count(void *ctx)
{
    (void)ctx;
    return IMG_SECTORS;
}

static const odfs_media_ops_t mem_media_ops = {
    .read_sectors = mem_read_sectors,
    .sector_size  = mem_sector_size,
    .sector_count = mem_sector_count,
};

static odfs_err_t mount_mem(uint8_t *img, odfs_mount_t *mnt)
{
    odfs_media_t media;

    memset(&media, 0, sizeof(media));
    media.ops = &mem_media_ops;
    media.ctx = img;
    return odfs_mount(&media, NULL, NULL, mnt);
}

static void put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* ---- B-tree node builder ---- */

typedef struct bt_builder {
    uint8_t *node;
    uint32_t node_size;
    uint16_t nrecs;
    uint16_t free_off;
} bt_builder_t;

static void bt_init(bt_builder_t *b, uint8_t *node, uint32_t node_size,
                    uint8_t kind, uint32_t flink, uint32_t blink)
{
    memset(node, 0, node_size);
    put_be32(&node[0], flink);
    put_be32(&node[4], blink);
    node[8] = kind;
    node[9] = 1;
    b->node = node;
    b->node_size = node_size;
    b->nrecs = 0;
    b->free_off = 14;
    put_be16(&node[node_size - 2], b->free_off);
}

static uint8_t *bt_add(bt_builder_t *b, uint16_t rec_len)
{
    uint8_t *p = b->node + b->free_off;

    b->free_off = (uint16_t)(b->free_off + ((rec_len + 1u) & ~1u));
    b->nrecs++;
    put_be16(&b->node[10], b->nrecs);
    put_be16(&b->node[b->node_size - 2u * (b->nrecs + 1u)], b->free_off);
    return p;
}

static void bt_header(uint8_t *node, uint32_t node_size, uint32_t root,
                      uint32_t total_nodes)
{
    bt_builder_t b;
    uint8_t *hdr;

    bt_init(&b, node, node_size, HFS_NODE_HEADER, 0, 0);
    hdr = bt_add(&b, 106);
    put_be16(&hdr[0], 1);                 /* depth */
    put_be32(&hdr[2], root);              /* root node */
    put_be16(&hdr[18], (uint16_t)node_size);
    put_be32(&hdr[22], total_nodes);
}

/* ---- HFS image: MDB at byte 1024, 512-byte allocation blocks ---- */

#define HFS_AB_BASE 2048u   /* drAlBlSt = 4 */

static uint8_t *hfs_image(uint16_t cat_blocks)
{
    uint8_t *img = odfs_calloc(1, IMG_BYTES);
    uint8_t *mdb;

    if (!img)
        return NULL;
    mdb = img + 1024;
    put_be16(&mdb[HFS_MDB_SIGWORD], HFS_MDB_SIG);
    put_be16(&mdb[HFS_MDB_NMALBLKS], (IMG_BYTES - HFS_AB_BASE) / 512u);
    put_be32(&mdb[HFS_MDB_ALBLKSIZ], 512);
    put_be16(&mdb[HFS_MDB_ALBLST], 4);
    mdb[HFS_MDB_VOLNAMELEN] = 7;
    memcpy(&mdb[HFS_MDB_VOLNAME], "HFSTEST", 7);
    put_be16(&mdb[HFS_MDB_CTEXTREC + 2], cat_blocks); /* catalog at AB 0 */
    return img;
}

static uint8_t *hfs_ab(uint8_t *img, uint32_t ab)
{
    return img + HFS_AB_BASE + ab * 512u;
}

static void hfs_add_file(bt_builder_t *b, uint32_t parent, const char *name,
                         uint32_t cnid, uint32_t size,
                         const hfs_extent_t exts[3])
{
    uint8_t nlen = (uint8_t)strlen(name);
    uint16_t key_bytes = (uint16_t)((1u + 6u + nlen + 1u) & ~1u);
    uint8_t *rec = bt_add(b, (uint16_t)(key_bytes + 102u));
    uint8_t *data = rec + key_bytes;

    rec[0] = (uint8_t)(6 + nlen);
    put_be32(&rec[2], parent);
    rec[6] = nlen;
    memcpy(&rec[7], name, nlen);

    data[0] = HFS_CAT_FILE;
    put_be32(&data[20], cnid);
    put_be32(&data[26], size);
    for (int i = 0; exts && i < 3; i++) {
        put_be16(&data[74 + i * 4], (uint16_t)exts[i].start_block);
        put_be16(&data[76 + i * 4], (uint16_t)exts[i].block_count);
    }
}

static void put_be64(uint8_t *p, uint64_t v)
{
    put_be32(p, (uint32_t)(v >> 32));
    put_be32(p + 4, (uint32_t)v);
}

/* ---- HFS+ volume at part: 512-byte blocks, catalog from block 4 ---- */

static uint8_t *hfsp_volume(uint8_t *part, uint32_t cat_blocks)
{
    uint8_t *vh = part + HFSPLUS_VH_OFFSET;

    put_be16(&vh[0], HFSPLUS_SIG);
    put_be16(&vh[2], 4);                  /* version */
    put_be32(&vh[40], 512);               /* blockSize */
    put_be32(&vh[44], 64);                /* totalBlocks */
    put_be64(&vh[272], (uint64_t)cat_blocks * 512u);
    put_be32(&vh[272 + 16], 4);           /* catalog extent 0 */
    put_be32(&vh[272 + 20], cat_blocks);
    return part + 4 * 512;                /* catalog node 0 */
}

static size_t utf16_ascii(uint8_t *dst, const char *src)
{
    size_t n = strlen(src);

    for (size_t i = 0; i < n; i++) {
        dst[i * 2] = 0;
        dst[i * 2 + 1] = (uint8_t)src[i];
    }
    return n;
}

static void hfsp_add_thread(bt_builder_t *b, uint32_t cnid,
                            uint32_t parent, const char *name)
{
    uint16_t ulen = (uint16_t)strlen(name);
    uint8_t *rec = bt_add(b, (uint16_t)(8 + 10 + ulen * 2));
    uint8_t *data = rec + 8;

    put_be16(&rec[0], 6);
    put_be32(&rec[2], cnid);
    put_be16(&data[0], HFS_CAT_DTHREAD);
    put_be32(&data[4], parent);
    put_be16(&data[8], ulen);
    utf16_ascii(&data[10], name);
}

/* add a file or folder record named by UTF-16BE uname; returns its data */
static uint8_t *hfsp_add_rec(bt_builder_t *b, uint32_t parent,
                             const uint8_t *uname, uint16_t ulen,
                             uint16_t type, uint32_t cnid)
{
    uint16_t klen = (uint16_t)(6 + ulen * 2);
    uint16_t dlen = type == HFS_CAT_DIR ? 88 : 248;
    uint8_t *rec = bt_add(b, (uint16_t)(2 + klen + dlen));
    uint8_t *data = rec + 2 + klen;

    put_be16(&rec[0], klen);
    put_be32(&rec[2], parent);
    put_be16(&rec[6], ulen);
    memcpy(&rec[8], uname, (size_t)ulen * 2);
    put_be16(&data[0], type);
    put_be32(&data[8], cnid);
    return data;
}

static uint8_t *hfsp_add_file(bt_builder_t *b, uint32_t parent,
                              const char *name, uint32_t cnid,
                              uint64_t size, uint32_t start, uint32_t count)
{
    uint8_t uname[128];
    uint16_t ulen = (uint16_t)utf16_ascii(uname, name);
    uint8_t *data = hfsp_add_rec(b, parent, uname, ulen, HFS_CAT_FILE, cnid);

    put_be64(&data[88], size);
    put_be32(&data[104], start);
    put_be32(&data[108], count);
    return data;
}

typedef struct entry_list {
    int count;
    odfs_node_t nodes[16];
} entry_list_t;

static odfs_err_t collect_cb(const odfs_node_t *entry, void *ctx)
{
    entry_list_t *list = ctx;

    if (list->count < 16)
        list->nodes[list->count++] = *entry;
    return ODFS_OK;
}

static odfs_err_t list_root(odfs_mount_t *mnt, entry_list_t *list)
{
    memset(list, 0, sizeof(*list));
    return odfs_readdir(mnt, &mnt->root, collect_cb, list, NULL);
}

/* ---- B-tree loop guards ---- */

TEST(hfs_leaf_self_link_is_rejected)
{
    uint8_t *img = hfs_image(2);
    odfs_mount_t mnt;
    entry_list_t list;
    bt_builder_t b;

    ASSERT(img != NULL);
    bt_header(hfs_ab(img, 0), 512, 1, 2);
    bt_init(&b, hfs_ab(img, 1), 512, HFS_NODE_LEAF, 1, 0);
    hfs_add_file(&b, HFS_CNID_ROOT_DIR, "A", 16, 0, NULL);

    ASSERT_OK(mount_mem(img, &mnt));
    ASSERT_EQ(list_root(&mnt, &list), ODFS_ERR_CORRUPT);
    odfs_unmount(&mnt);
    odfs_free(img);
}

TEST(hfs_leaf_cycle_is_bounded)
{
    uint8_t *img = hfs_image(3);
    odfs_mount_t mnt;
    entry_list_t list;
    bt_builder_t b;

    ASSERT(img != NULL);
    /* leaves 1 and 2 link to each other in both directions */
    bt_header(hfs_ab(img, 0), 512, 1, 3);
    bt_init(&b, hfs_ab(img, 1), 512, HFS_NODE_LEAF, 2, 2);
    hfs_add_file(&b, HFS_CNID_ROOT_DIR, "A", 16, 0, NULL);
    bt_init(&b, hfs_ab(img, 2), 512, HFS_NODE_LEAF, 1, 1);
    hfs_add_file(&b, HFS_CNID_ROOT_DIR, "B", 17, 0, NULL);

    ASSERT_OK(mount_mem(img, &mnt));
    ASSERT_EQ(list_root(&mnt, &list), ODFS_ERR_LOOP);
    odfs_unmount(&mnt);
    odfs_free(img);
}

TEST(hfs_index_self_link_is_rejected)
{
    uint8_t *img = hfs_image(2);
    odfs_mount_t mnt;
    entry_list_t list;
    bt_builder_t b;
    uint8_t *rec;

    ASSERT(img != NULL);
    bt_header(hfs_ab(img, 0), 512, 1, 2);
    bt_init(&b, hfs_ab(img, 1), 512, HFS_NODE_INDEX, 0, 0);
    rec = bt_add(&b, 12);
    rec[0] = 7;
    put_be32(&rec[2], HFS_CNID_ROOT_DIR);
    put_be32(&rec[8], 1);                 /* child: itself */

    ASSERT_OK(mount_mem(img, &mnt));
    ASSERT_EQ(list_root(&mnt, &list), ODFS_ERR_LOOP);
    odfs_unmount(&mnt);
    odfs_free(img);
}

/* ---- index records ---- */

TEST(hfs_index_pointer_follows_padded_key)
{
    uint8_t *img = hfs_image(3);
    odfs_mount_t mnt;
    entry_list_t list;
    bt_builder_t b;
    uint8_t *rec;

    ASSERT(img != NULL);
    bt_header(hfs_ab(img, 0), 512, 1, 3);

    /* variable-length index key: keyLength 8 is even, so the key takes
     * nine bytes and the child pointer starts after a pad byte */
    bt_init(&b, hfs_ab(img, 1), 512, HFS_NODE_INDEX, 0, 0);
    rec = bt_add(&b, 14);
    rec[0] = 8;
    put_be32(&rec[2], HFS_CNID_ROOT_DIR);
    rec[6] = 2;
    memcpy(&rec[7], "FR", 2);
    put_be32(&rec[10], 2);                /* child: leaf 2 */

    bt_init(&b, hfs_ab(img, 2), 512, HFS_NODE_LEAF, 0, 0);
    hfs_add_file(&b, HFS_CNID_ROOT_DIR, "FRAG.BIN", 16, 0, NULL);

    ASSERT_OK(mount_mem(img, &mnt));
    ASSERT_OK(list_root(&mnt, &list));
    ASSERT_EQ(list.count, 1);
    ASSERT_STR_EQ(list.nodes[0].name, "FRAG.BIN");
    odfs_unmount(&mnt);
    odfs_free(img);
}

/* ---- node ids ---- */

TEST(hfs_node_ids_are_catalog_ids)
{
    uint8_t *img = hfs_image(2);
    odfs_mount_t mnt;
    odfs_node_t a, b2;
    bt_builder_t b;

    ASSERT(img != NULL);
    bt_header(hfs_ab(img, 0), 512, 1, 2);
    bt_init(&b, hfs_ab(img, 1), 512, HFS_NODE_LEAF, 0, 0);
    hfs_add_file(&b, HFS_CNID_ROOT_DIR, "FILE.TXT", 23, 0, NULL);

    ASSERT_OK(mount_mem(img, &mnt));
    ASSERT_OK(odfs_lookup(&mnt, &mnt.root, "FILE.TXT", &a));
    ASSERT_OK(odfs_lookup(&mnt, &mnt.root, "file.txt", &b2));
    ASSERT_EQ(a.id, 23u);
    ASSERT_EQ(b2.id, 23u);
    odfs_unmount(&mnt);
    odfs_free(img);
}

/* ---- Apple Partition Map ---- */

static void apm_entry(uint8_t *img, uint32_t idx, uint32_t count,
                      uint32_t start, const char *type)
{
    uint8_t *pm = img + idx * 512u;

    put_be16(&pm[0], HFS_APM_SIG);
    put_be32(&pm[4], count);
    put_be32(&pm[8], start);
    put_be32(&pm[12], 64);
    memcpy(&pm[48], type, strlen(type));
}

TEST(hfs_in_second_apm_entry)
{
    uint8_t *img = odfs_calloc(1, IMG_BYTES);
    uint8_t *part;
    uint8_t *mdb;
    odfs_mount_t mnt;
    entry_list_t list;
    bt_builder_t b;

    ASSERT(img != NULL);
    /* layout written by mkisofs -hfs -part: the map, then Apple_HFS */
    apm_entry(img, 1, 2, 1, "Apple_partition_map");
    apm_entry(img, 2, 2, 16, "Apple_HFS");

    part = img + 16 * 512;
    mdb = part + 1024;
    put_be16(&mdb[HFS_MDB_SIGWORD], HFS_MDB_SIG);
    put_be16(&mdb[HFS_MDB_NMALBLKS], 64);
    put_be32(&mdb[HFS_MDB_ALBLKSIZ], 512);
    put_be16(&mdb[HFS_MDB_ALBLST], 4);
    put_be16(&mdb[HFS_MDB_CTEXTREC + 2], 2);

    bt_header(part + 2048, 512, 1, 2);
    bt_init(&b, part + 2560, 512, HFS_NODE_LEAF, 0, 0);
    hfs_add_file(&b, HFS_CNID_ROOT_DIR, "APM.TXT", 16, 0, NULL);

    ASSERT_OK(mount_mem(img, &mnt));
    ASSERT_EQ(mnt.active_backend, ODFS_BACKEND_HFS);
    ASSERT_OK(list_root(&mnt, &list));
    ASSERT_EQ(list.count, 1);
    ASSERT_STR_EQ(list.nodes[0].name, "APM.TXT");
    odfs_unmount(&mnt);
    odfs_free(img);
}

TEST(hfsplus_in_apm_partition_past_scan_window)
{
    uint8_t *img = odfs_calloc(1, IMG_BYTES);
    uint8_t *part;
    uint8_t *cat;
    odfs_mount_t mnt;
    odfs_node_t file;
    bt_builder_t b;
    char buf[8];
    size_t len = sizeof(buf);

    ASSERT(img != NULL);
    /* the partition starts past the 256 KiB signature scan */
    apm_entry(img, 1, 3, 1, "Apple_partition_map");
    apm_entry(img, 2, 3, 64, "Apple_Driver43");
    apm_entry(img, 3, 3, 600, "Apple_HFS");

    part = img + 600 * 512;
    cat = hfsp_volume(part, 2);
    bt_header(cat, 512, 1, 2);
    bt_init(&b, cat + 512, 512, HFS_NODE_LEAF, 0, 0);
    hfsp_add_thread(&b, HFS_CNID_ROOT_DIR, 1, "Far");
    hfsp_add_file(&b, HFS_CNID_ROOT_DIR, "far.txt", 16, 3, 8, 1);
    memcpy(part + 8 * 512, "FAR", 3);

    ASSERT_OK(mount_mem(img, &mnt));
    ASSERT_EQ(mnt.active_backend, ODFS_BACKEND_HFSPLUS);
    ASSERT_STR_EQ(mnt.volume_name, "Far");
    ASSERT_OK(odfs_lookup(&mnt, &mnt.root, "far.txt", &file));
    ASSERT_OK(odfs_read(&mnt, &file, 0, buf, &len));
    ASSERT_EQ(len, 3u);
    ASSERT(memcmp(buf, "FAR", 3) == 0);
    odfs_unmount(&mnt);
    odfs_free(img);
}

/* ---- HFS wrapper ---- */

TEST(hfsplus_mounts_volume_embedded_in_hfs_wrapper)
{
    uint8_t *img = odfs_calloc(1, IMG_BYTES);
    uint8_t *mdb;
    uint8_t *part;
    uint8_t *cat;
    odfs_mount_t mnt;
    entry_list_t list;
    bt_builder_t b;

    ASSERT(img != NULL);
    /* wrapper MDB with 4 KiB allocation blocks; the HFS+ volume starts
     * at allocation block 64, past the 256 KiB signature scan */
    mdb = img + 1024;
    put_be16(&mdb[HFS_MDB_SIGWORD], HFS_MDB_SIG);
    put_be16(&mdb[HFS_MDB_NMALBLKS], 78);
    put_be32(&mdb[HFS_MDB_ALBLKSIZ], 4096);
    put_be16(&mdb[HFS_MDB_ALBLST], 4);
    put_be16(&mdb[HFS_MDB_EMBEDSIG], HFSPLUS_SIG);
    put_be16(&mdb[HFS_MDB_EMBEDEXT], 64);
    put_be16(&mdb[HFS_MDB_EMBEDEXT + 2], 14);

    part = img + 2048 + 64 * 4096;
    cat = hfsp_volume(part, 2);
    bt_header(cat, 512, 1, 2);
    bt_init(&b, cat + 512, 512, HFS_NODE_LEAF, 0, 0);
    hfsp_add_thread(&b, HFS_CNID_ROOT_DIR, 1, "Wrapped");
    hfsp_add_file(&b, HFS_CNID_ROOT_DIR, "inner.txt", 16, 0, 0, 0);

    ASSERT_OK(mount_mem(img, &mnt));
    ASSERT_EQ(mnt.active_backend, ODFS_BACKEND_HFSPLUS);
    ASSERT_STR_EQ(mnt.volume_name, "Wrapped");
    ASSERT_OK(list_root(&mnt, &list));
    ASSERT_EQ(list.count, 1);
    ASSERT_STR_EQ(list.nodes[0].name, "inner.txt");
    odfs_unmount(&mnt);
    odfs_free(img);
}

/* ---- Volume Header validation ---- */

TEST(hfsplus_probe_rejects_stray_signatures)
{
    uint8_t *img = odfs_calloc(1, IMG_BYTES);
    odfs_media_t media;
    odfs_cache_t cache;

    ASSERT(img != NULL);
    memset(&media, 0, sizeof(media));
    media.ops = &mem_media_ops;
    media.ctx = img;
    ASSERT_OK(odfs_cache_init(&cache, &media, 16));

    /* at byte 0 the volume would start 1024 bytes before the image */
    put_be16(&img[0], HFSPLUS_SIG);
    put_be16(&img[2], 4);
    put_be32(&img[40], 4096);
    /* unknown version */
    put_be16(&img[2048], HFSPLUS_SIG);
    put_be16(&img[2050], 99);
    put_be32(&img[2088], 4096);
    /* block size not a power of two */
    put_be16(&img[4096], HFSPLUS_SIG);
    put_be16(&img[4098], 4);
    put_be32(&img[4136], 1000);
    /* 'H+' in the middle of data */
    put_be16(&img[6144], HFSPLUS_SIG);

    ASSERT_EQ(hfsplus_backend_ops.probe(&cache, NULL, 0), ODFS_ERR_BAD_FORMAT);
    odfs_cache_destroy(&cache);
    odfs_free(img);
}

/* ---- volume name ---- */

TEST(hfs_volume_name_keeps_multibyte_characters)
{
    uint8_t *img = hfs_image(2);
    uint8_t *mdb;
    odfs_mount_t mnt;
    bt_builder_t b;

    ASSERT(img != NULL);
    /* 27 x Mac Roman 0xAA (U+2122), three UTF-8 bytes each */
    mdb = img + 1024;
    mdb[HFS_MDB_VOLNAMELEN] = 27;
    memset(&mdb[HFS_MDB_VOLNAME], 0xAA, 27);
    bt_header(hfs_ab(img, 0), 512, 1, 2);
    bt_init(&b, hfs_ab(img, 1), 512, HFS_NODE_LEAF, 0, 0);

    ASSERT_OK(mount_mem(img, &mnt));
    ASSERT_EQ(strlen(mnt.volume_name), 81u);
    ASSERT(memcmp(&mnt.volume_name[78], "\xE2\x84\xA2", 3) == 0);
    odfs_unmount(&mnt);
    odfs_free(img);
}

TEST_MAIN()
