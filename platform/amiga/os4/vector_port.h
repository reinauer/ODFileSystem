/*
 * vector_port.h - AmigaOS 4 filesystem vector-port frontend
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef ODFS_AMIGA_OS4_VECTOR_PORT_H
#define ODFS_AMIGA_OS4_VECTOR_PORT_H

#include <exec/types.h>
#include <dos/filehandler.h>

struct DosPacket;

const struct FileSystemVectors *odfs_os4_vector_template(void);
struct FileSystemVectorPort *odfs_os4_alloc_vector_port(APTR fs_private);
void odfs_os4_free_vector_port(struct FileSystemVectorPort *vp);

/*
 * Route a direct legacy DosPacket through the DOS packet emulator that
 * AllocDosObject() installed in the vector port. Results are placed in
 * the packet; the caller still replies it.
 */
void odfs_os4_emulate_packet(struct FileSystemVectorPort *vp,
                             struct DosPacket *pkt);

/*
 * Stop dos.library from vectoring new callers (sets the vector version
 * to zero). Must be called before DOS-visible shutdown teardown.
 */
void odfs_os4_invalidate_vector_port(struct FileSystemVectorPort *vp);

#endif /* ODFS_AMIGA_OS4_VECTOR_PORT_H */
