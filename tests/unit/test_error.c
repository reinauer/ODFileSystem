/*
 * test_error.c — tests for error code module
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "odfs/error.h"
#include "test_harness.h"

TEST(error_ok_string)
{
    ASSERT_STR_EQ(odfs_err_str(ODFS_OK), "OK");
}

TEST(error_known_codes)
{
    ASSERT_STR_EQ(odfs_err_str(ODFS_ERR_NOMEM), "out of memory");
    ASSERT_STR_EQ(odfs_err_str(ODFS_ERR_IO), "I/O error");
    ASSERT_STR_EQ(odfs_err_str(ODFS_ERR_NOT_FOUND), "not found");
    ASSERT_STR_EQ(odfs_err_str(ODFS_ERR_BAD_FORMAT), "bad format");
    ASSERT_STR_EQ(odfs_err_str(ODFS_ERR_READ_ONLY), "read-only filesystem");
    ASSERT_STR_EQ(odfs_err_str(ODFS_ERR_EOF), "end of file");
}

TEST(error_unknown_code)
{
    ASSERT_STR_EQ(odfs_err_str((odfs_err_t)999), "unknown error");
}

TEST_MAIN()
