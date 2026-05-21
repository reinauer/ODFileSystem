/*
 * vector_port.h - AmigaOS 4 filesystem vector-port frontend
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef ODFS_AMIGA_OS4_VECTOR_PORT_H
#define ODFS_AMIGA_OS4_VECTOR_PORT_H

#include <exec/types.h>
#include <dos/filehandler.h>

const struct FileSystemVectors *odfs_os4_vector_template(void);
struct FileSystemVectorPort *odfs_os4_alloc_vector_port(APTR fs_private);
void odfs_os4_free_vector_port(struct FileSystemVectorPort *vp);

#endif /* ODFS_AMIGA_OS4_VECTOR_PORT_H */
