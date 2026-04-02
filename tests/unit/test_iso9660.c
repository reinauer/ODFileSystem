/*
 * test_iso9660.c — tests for ISO 9660 backend
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "odfs/api.h"
#include "odfs/error.h"
#include "iso9660/iso9660.h"
#include "test_harness.h"

/* ---- endian / field helpers ---- */

TEST(iso_read_le16_basic)
{
    uint8_t buf[] = { 0x34, 0x12, 0x12, 0x34 };
    ASSERT_EQ(iso_read_le16(buf), 0x1234);
}

TEST(iso_read_le32_basic)
{
    uint8_t buf[] = { 0x78, 0x56, 0x34, 0x12, 0x12, 0x34, 0x56, 0x78 };
    ASSERT_EQ(iso_read_le32(buf), 0x12345678);
}

TEST(iso_read_le32_zero)
{
    uint8_t buf[8] = { 0 };
    ASSERT_EQ(iso_read_le32(buf), 0);
}

TEST(iso_read_le32_max)
{
    uint8_t buf[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    ASSERT_EQ(iso_read_le32(buf), 0xFFFFFFFF);
}

/* ---- PVD signature recognition ---- */

TEST(iso_pvd_signature)
{
    /* verify the standard identifier constant */
    ASSERT_EQ(ISO_STANDARD_ID_LEN, 5);
    ASSERT_EQ(memcmp(ISO_STANDARD_ID, "CD001", 5), 0);
}

TEST(iso_vd_type_constants)
{
    ASSERT_EQ(ISO_VD_TYPE_PRIMARY, 1);
    ASSERT_EQ(ISO_VD_TYPE_SUPPL, 2);
    ASSERT_EQ(ISO_VD_TYPE_TERM, 255);
}

/* ---- directory record flags ---- */

TEST(iso_dr_flag_bits)
{
    ASSERT_EQ(ISO_DR_FLAG_HIDDEN, 0x01);
    ASSERT_EQ(ISO_DR_FLAG_DIRECTORY, 0x02);
    ASSERT_EQ(ISO_DR_FLAG_ASSOCIATED, 0x04);
    ASSERT_EQ(ISO_DR_FLAG_MULTI_EXTENT, 0x80);
}

/* ---- backend ops table ---- */

TEST(iso_backend_ops_valid)
{
    ASSERT(iso9660_backend_ops.name != NULL);
    ASSERT_STR_EQ(iso9660_backend_ops.name, "iso9660");
    ASSERT_EQ(iso9660_backend_ops.backend_type, ODFS_BACKEND_ISO9660);
    ASSERT(iso9660_backend_ops.probe != NULL);
    ASSERT(iso9660_backend_ops.mount != NULL);
    ASSERT(iso9660_backend_ops.unmount != NULL);
    ASSERT(iso9660_backend_ops.readdir != NULL);
    ASSERT(iso9660_backend_ops.read != NULL);
    ASSERT(iso9660_backend_ops.lookup != NULL);
    ASSERT(iso9660_backend_ops.get_volume_name != NULL);
}

/* ---- PVD field offsets ---- */

TEST(iso_pvd_offsets)
{
    /* verify critical offsets match ECMA-119 */
    ASSERT_EQ(ISO_PVD_TYPE, 0);
    ASSERT_EQ(ISO_PVD_ID, 1);
    ASSERT_EQ(ISO_PVD_VOLUME_ID, 40);
    ASSERT_EQ(ISO_PVD_VOLUME_SPACE_SIZE, 80);
    ASSERT_EQ(ISO_PVD_LOGICAL_BLK_SIZE, 128);
    ASSERT_EQ(ISO_PVD_ROOT_DIR_RECORD, 156);
}

/* ---- directory record offsets ---- */

TEST(iso_dr_offsets)
{
    ASSERT_EQ(ISO_DR_LENGTH, 0);
    ASSERT_EQ(ISO_DR_EXTENT_LBA, 2);
    ASSERT_EQ(ISO_DR_DATA_LENGTH, 10);
    ASSERT_EQ(ISO_DR_DATE, 18);
    ASSERT_EQ(ISO_DR_FLAGS, 25);
    ASSERT_EQ(ISO_DR_NAME_LEN, 32);
    ASSERT_EQ(ISO_DR_NAME, 33);
}

TEST_MAIN()
