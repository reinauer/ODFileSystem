/*
 * odfs/error.h — error codes
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef ODFS_ERROR_H
#define ODFS_ERROR_H

typedef enum odfs_err {
    ODFS_OK = 0,

    /* generic errors */
    ODFS_ERR_NOMEM,          /* allocation failed */
    ODFS_ERR_IO,             /* device I/O error */
    ODFS_ERR_INVAL,          /* invalid argument */
    ODFS_ERR_RANGE,          /* value out of range */
    ODFS_ERR_OVERFLOW,       /* overflow detected */

    /* media errors */
    ODFS_ERR_NO_MEDIA,       /* no media present */
    ODFS_ERR_BAD_SECTOR,     /* unreadable sector */
    ODFS_ERR_MEDIA_CHANGED,  /* media changed unexpectedly */

    /* format errors */
    ODFS_ERR_NOT_FOUND,      /* object not found */
    ODFS_ERR_BAD_FORMAT,     /* unrecognized or corrupt format */
    ODFS_ERR_UNSUPPORTED,    /* recognized but unsupported feature */
    ODFS_ERR_CORRUPT,        /* internal inconsistency detected */
    ODFS_ERR_LOOP,           /* loop detected in on-disc structure */
    ODFS_ERR_NAME_TOO_LONG,  /* name exceeds limits */

    /* handler errors */
    ODFS_ERR_NOT_DIR,        /* expected directory */
    ODFS_ERR_IS_DIR,         /* expected file, got directory */
    ODFS_ERR_READ_ONLY,      /* write attempted on read-only fs */
    ODFS_ERR_TOO_MANY_OPEN,  /* open handle limit reached */
    ODFS_ERR_EOF,            /* end of file / data */

    ODFS_ERR__COUNT
} odfs_err_t;

const char *odfs_err_str(odfs_err_t err);

#endif /* ODFS_ERROR_H */
