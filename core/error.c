/*
 * error.c — error code to string mapping
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "odfs/error.h"

static const char *error_strings[] = {
    [ODFS_OK]                = "OK",
    [ODFS_ERR_NOMEM]         = "out of memory",
    [ODFS_ERR_IO]            = "I/O error",
    [ODFS_ERR_INVAL]         = "invalid argument",
    [ODFS_ERR_RANGE]         = "value out of range",
    [ODFS_ERR_OVERFLOW]      = "overflow",
    [ODFS_ERR_NO_MEDIA]      = "no media",
    [ODFS_ERR_BAD_SECTOR]    = "bad sector",
    [ODFS_ERR_MEDIA_CHANGED] = "media changed",
    [ODFS_ERR_NOT_FOUND]     = "not found",
    [ODFS_ERR_BAD_FORMAT]    = "bad format",
    [ODFS_ERR_UNSUPPORTED]   = "unsupported",
    [ODFS_ERR_CORRUPT]       = "corrupt",
    [ODFS_ERR_LOOP]          = "loop detected",
    [ODFS_ERR_NAME_TOO_LONG] = "name too long",
    [ODFS_ERR_NOT_DIR]       = "not a directory",
    [ODFS_ERR_IS_DIR]        = "is a directory",
    [ODFS_ERR_READ_ONLY]     = "read-only filesystem",
    [ODFS_ERR_TOO_MANY_OPEN] = "too many open files",
    [ODFS_ERR_EOF]           = "end of file",
};

const char *odfs_err_str(odfs_err_t err)
{
    if (err >= 0 && err < ODFS_ERR__COUNT)
        return error_strings[err];
    return "unknown error";
}
