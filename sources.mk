# Shared parser sources for standalone and AROS builds.
# SPDX-License-Identifier: BSD-2-Clause

ODFS_CORE_SRCS = \
    core/error.c \
    core/log.c \
    core/node.c \
    core/timestamp.c \
    core/namefix.c \
    core/cache_block.c \
    core/cache_meta.c \
    core/charset.c \
    core/ancestry.c \
    core/mount.c \
    core/session.c \
    backends/iso9660/iso9660.c \
    backends/rock_ridge/rock_ridge.c \
    backends/joliet/joliet.c \
    backends/udf/udf.c \
    backends/hfs/hfs.c \
    backends/hfsplus/hfsplus.c \
    backends/cdda/cdda.c
