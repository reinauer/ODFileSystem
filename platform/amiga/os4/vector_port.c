/*
 * vector_port.c - AmigaOS 4 filesystem vector-port frontend
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "vector_port.h"

#include <dos/dos.h>
#include <dos/dostags.h>
#include <exec/nodes.h>
#include <utility/tagitem.h>

#include <proto/dos.h>

#include <stddef.h>

static void set_unsupported(struct FSVP *vp, int32 *res2)
{
    (void)vp;

    if (res2)
        *res2 = ERROR_ACTION_NOT_KNOWN;
}

static struct Lock *vp_lock(struct FSVP *vp,
                            int32 *res2,
                            struct Lock *rel_lock,
                            CONST_STRPTR obj,
                            int32 mode)
{
    set_unsupported(vp, res2);
    (void)rel_lock;
    (void)obj;
    (void)mode;
    return NULL;
}

static int32 vp_unlock(struct FSVP *vp, int32 *res2, struct Lock *lock)
{
    set_unsupported(vp, res2);
    (void)lock;
    return DOSFALSE;
}

static struct Lock *vp_dup_lock(struct FSVP *vp,
                                int32 *res2,
                                struct Lock *lock)
{
    set_unsupported(vp, res2);
    (void)lock;
    return NULL;
}

static struct Lock *vp_create_dir(struct FSVP *vp,
                                  int32 *res2,
                                  struct Lock *rel_lock,
                                  CONST_STRPTR obj)
{
    set_unsupported(vp, res2);
    (void)rel_lock;
    (void)obj;
    return NULL;
}

static struct Lock *vp_parent_dir(struct FSVP *vp,
                                  int32 *res2,
                                  struct Lock *dirlock)
{
    set_unsupported(vp, res2);
    (void)dirlock;
    return NULL;
}

static struct Lock *vp_dup_lock_from_fh(struct FSVP *vp,
                                        int32 *res2,
                                        struct FileHandle *filehandle)
{
    set_unsupported(vp, res2);
    (void)filehandle;
    return NULL;
}

static int32 vp_open_from_lock(struct FSVP *vp,
                               int32 *res2,
                               struct FileHandle *file,
                               struct Lock *lock)
{
    set_unsupported(vp, res2);
    (void)file;
    (void)lock;
    return DOSFALSE;
}

static struct Lock *vp_parent_of_fh(struct FSVP *vp,
                                    int32 *res2,
                                    struct FileHandle *file)
{
    set_unsupported(vp, res2);
    (void)file;
    return NULL;
}

static int32 vp_open(struct FSVP *vp,
                     int32 *res2,
                     struct FileHandle *fh,
                     struct Lock *rel_dir,
                     CONST_STRPTR obj,
                     int32 mode)
{
    set_unsupported(vp, res2);
    (void)fh;
    (void)rel_dir;
    (void)obj;
    (void)mode;
    return DOSFALSE;
}

static int32 vp_close(struct FSVP *vp, int32 *res2, struct FileHandle *file)
{
    set_unsupported(vp, res2);
    (void)file;
    return DOSFALSE;
}

static int32 vp_delete(struct FSVP *vp,
                       int32 *res2,
                       struct Lock *rel_dirlock,
                       CONST_STRPTR obj)
{
    set_unsupported(vp, res2);
    (void)rel_dirlock;
    (void)obj;
    return DOSFALSE;
}

static int32 vp_read(struct FSVP *vp,
                     int32 *res2,
                     struct FileHandle *file,
                     STRPTR buffer,
                     int32 numbytes)
{
    set_unsupported(vp, res2);
    (void)file;
    (void)buffer;
    (void)numbytes;
    return 0;
}

static int32 vp_write(struct FSVP *vp,
                      int32 *res2,
                      struct FileHandle *file,
                      STRPTR buffer,
                      int32 numbytes)
{
    set_unsupported(vp, res2);
    (void)file;
    (void)buffer;
    (void)numbytes;
    return 0;
}

static int32 vp_flush(struct FSVP *vp, int32 *res2)
{
    set_unsupported(vp, res2);
    return DOSFALSE;
}

static int32 vp_change_file_position(struct FSVP *vp,
                                     int32 *res2,
                                     struct FileHandle *file,
                                     int32 mode,
                                     int64 position)
{
    set_unsupported(vp, res2);
    (void)file;
    (void)mode;
    (void)position;
    return DOSFALSE;
}

static int32 vp_change_file_size(struct FSVP *vp,
                                 int32 *res2,
                                 struct FileHandle *file,
                                 int32 mode,
                                 int64 size)
{
    set_unsupported(vp, res2);
    (void)file;
    (void)mode;
    (void)size;
    return DOSFALSE;
}

static int64 vp_get_file_position(struct FSVP *vp,
                                  int32 *res2,
                                  struct FileHandle *file)
{
    set_unsupported(vp, res2);
    (void)file;
    return 0;
}

static int64 vp_get_file_size(struct FSVP *vp,
                              int32 *res2,
                              struct FileHandle *file)
{
    set_unsupported(vp, res2);
    (void)file;
    return 0;
}

static int32 vp_change_lock_mode(struct FSVP *vp,
                                 int32 *res2,
                                 struct Lock *lock,
                                 int32 new_lock_mode)
{
    set_unsupported(vp, res2);
    (void)lock;
    (void)new_lock_mode;
    return DOSFALSE;
}

static int32 vp_change_file_mode(struct FSVP *vp,
                                 int32 *res2,
                                 struct FileHandle *fh,
                                 int32 new_lock_mode)
{
    set_unsupported(vp, res2);
    (void)fh;
    (void)new_lock_mode;
    return DOSFALSE;
}

static int32 vp_set_date(struct FSVP *vp,
                         int32 *res2,
                         struct Lock *rel_dirlock,
                         CONST_STRPTR name,
                         const struct DateStamp *ds)
{
    set_unsupported(vp, res2);
    (void)rel_dirlock;
    (void)name;
    (void)ds;
    return DOSFALSE;
}

static int32 vp_set_protection(struct FSVP *vp,
                               int32 *res2,
                               struct Lock *rel_dirlock,
                               CONST_STRPTR name,
                               uint32 mask)
{
    set_unsupported(vp, res2);
    (void)rel_dirlock;
    (void)name;
    (void)mask;
    return DOSFALSE;
}

static int32 vp_set_comment(struct FSVP *vp,
                            int32 *res2,
                            struct Lock *rel_dirlock,
                            CONST_STRPTR name,
                            CONST_STRPTR comment)
{
    set_unsupported(vp, res2);
    (void)rel_dirlock;
    (void)name;
    (void)comment;
    return DOSFALSE;
}

static int32 vp_set_group(struct FSVP *vp,
                          int32 *res2,
                          struct Lock *rel_dirlock,
                          CONST_STRPTR name,
                          uint32 group)
{
    set_unsupported(vp, res2);
    (void)rel_dirlock;
    (void)name;
    (void)group;
    return DOSFALSE;
}

static int32 vp_set_user(struct FSVP *vp,
                         int32 *res2,
                         struct Lock *rel_dirlock,
                         CONST_STRPTR name,
                         uint32 user)
{
    set_unsupported(vp, res2);
    (void)rel_dirlock;
    (void)name;
    (void)user;
    return DOSFALSE;
}

static int32 vp_rename(struct FSVP *vp,
                       int32 *res2,
                       struct Lock *src_rel,
                       CONST_STRPTR src,
                       struct Lock *dst_rel,
                       CONST_STRPTR dst)
{
    set_unsupported(vp, res2);
    (void)src_rel;
    (void)src;
    (void)dst_rel;
    (void)dst;
    return DOSFALSE;
}

static int32 vp_create_soft_link(struct FSVP *vp,
                                 int32 *res2,
                                 struct Lock *rel_dirlock,
                                 CONST_STRPTR linkname,
                                 CONST_STRPTR dest_obj)
{
    set_unsupported(vp, res2);
    (void)rel_dirlock;
    (void)linkname;
    (void)dest_obj;
    return DOSFALSE;
}

static int32 vp_create_hard_link(struct FSVP *vp,
                                 int32 *res2,
                                 struct Lock *rel_dirlock,
                                 CONST_STRPTR linkname,
                                 struct Lock *dest_obj)
{
    set_unsupported(vp, res2);
    (void)rel_dirlock;
    (void)linkname;
    (void)dest_obj;
    return DOSFALSE;
}

static int32 vp_read_soft_link(struct FSVP *vp,
                               int32 *res2,
                               struct Lock *rel_dir,
                               CONST_STRPTR linkname,
                               STRPTR buf,
                               int32 bufsize)
{
    set_unsupported(vp, res2);
    (void)rel_dir;
    (void)linkname;
    (void)buf;
    (void)bufsize;
    return DOSFALSE;
}

static int32 vp_same_lock(struct FSVP *vp,
                          int32 *res2,
                          struct Lock *lock1,
                          struct Lock *lock2)
{
    set_unsupported(vp, res2);
    (void)lock1;
    (void)lock2;
    return DOSFALSE;
}

static int32 vp_same_file(struct FSVP *vp,
                          int32 *res2,
                          struct FileHandle *fh1,
                          struct FileHandle *fh2)
{
    set_unsupported(vp, res2);
    (void)fh1;
    (void)fh2;
    return DOSFALSE;
}

static int32 vp_filesystem_attr(struct FSVP *vp,
                                int32 *res2,
                                struct TagItem *taglist)
{
    set_unsupported(vp, res2);
    (void)taglist;
    return DOSFALSE;
}

static int32 vp_volume_info_data(struct FSVP *vp,
                                 int32 *res2,
                                 struct InfoData *info)
{
    set_unsupported(vp, res2);
    (void)info;
    return DOSFALSE;
}

static int32 vp_device_info_data(struct FSVP *vp,
                                 int32 *res2,
                                 struct InfoData *info)
{
    set_unsupported(vp, res2);
    (void)info;
    return DOSFALSE;
}

static struct ExamineData *vp_examine_obj(struct FSVP *vp,
                                          int32 *res2,
                                          struct Lock *lock,
                                          CONST_STRPTR object)
{
    set_unsupported(vp, res2);
    (void)lock;
    (void)object;
    return NULL;
}

static struct ExamineData *vp_examine_lock(struct FSVP *vp,
                                           int32 *res2,
                                           struct Lock *lock)
{
    set_unsupported(vp, res2);
    (void)lock;
    return NULL;
}

static struct ExamineData *vp_examine_file(struct FSVP *vp,
                                           int32 *res2,
                                           struct FileHandle *file)
{
    set_unsupported(vp, res2);
    (void)file;
    return NULL;
}

static int32 vp_examine_dir(struct FSVP *vp,
                            int32 *res2,
                            struct PRIVATE_ExamineDirContext *ctx)
{
    set_unsupported(vp, res2);
    (void)ctx;
    return DOSFALSE;
}

static int32 vp_inhibit(struct FSVP *vp, int32 *res2, int32 inhibit_state)
{
    set_unsupported(vp, res2);
    (void)inhibit_state;
    return DOSFALSE;
}

static int32 vp_write_protect(struct FSVP *vp,
                              int32 *res2,
                              int32 wp_state,
                              uint32 passkey)
{
    set_unsupported(vp, res2);
    (void)wp_state;
    (void)passkey;
    return DOSFALSE;
}

static int32 vp_format(struct FSVP *vp,
                       int32 *res2,
                       CONST_STRPTR new_volname,
                       uint32 dostype,
                       uint32 spare)
{
    set_unsupported(vp, res2);
    (void)new_volname;
    (void)dostype;
    (void)spare;
    return DOSFALSE;
}

static int32 vp_serialize(struct FSVP *vp, int32 *res2)
{
    set_unsupported(vp, res2);
    return DOSFALSE;
}

static int32 vp_relabel(struct FSVP *vp,
                        int32 *res2,
                        CONST_STRPTR new_volumename)
{
    set_unsupported(vp, res2);
    (void)new_volumename;
    return DOSFALSE;
}

static int32 vp_add_notify(struct FSVP *vp,
                           int32 *res2,
                           struct NotifyRequest *nr)
{
    set_unsupported(vp, res2);
    (void)nr;
    return DOSFALSE;
}

static int32 vp_remove_notify(struct FSVP *vp,
                              int32 *res2,
                              struct NotifyRequest *nr)
{
    set_unsupported(vp, res2);
    (void)nr;
    return DOSFALSE;
}

static int32 vp_lock_record(struct FSVP *vp,
                            int32 *res2,
                            struct FileHandle *file,
                            int64 offset,
                            int64 length,
                            uint32 mode,
                            uint32 timeout)
{
    set_unsupported(vp, res2);
    (void)file;
    (void)offset;
    (void)length;
    (void)mode;
    (void)timeout;
    return DOSFALSE;
}

static int32 vp_unlock_record(struct FSVP *vp,
                              int32 *res2,
                              struct FileHandle *file,
                              int64 offset,
                              int64 length)
{
    set_unsupported(vp, res2);
    (void)file;
    (void)offset;
    (void)length;
    return DOSFALSE;
}

static const struct FileSystemVectors odfs_os4_vectors = {
    .StructSize = sizeof(struct FileSystemVectors),
    .Version = FS_VECTORPORT_VERSION,
    .FSPrivate = NULL,
    .Reserved = {0, 0, 0},
    .DOSPrivate = NULL,
    .DOSEmulatePacket = NULL,
    .FSLock = vp_lock,
    .FSUnLock = vp_unlock,
    .FSDupLock = vp_dup_lock,
    .FSCreateDir = vp_create_dir,
    .FSParentDir = vp_parent_dir,
    .FSDupLockFromFH = vp_dup_lock_from_fh,
    .FSOpenFromLock = vp_open_from_lock,
    .FSParentOfFH = vp_parent_of_fh,
    .FSOpen = vp_open,
    .FSClose = vp_close,
    .FSDelete = vp_delete,
    .FSRead = vp_read,
    .FSWrite = vp_write,
    .FSFlush = vp_flush,
    .FSChangeFilePosition = vp_change_file_position,
    .FSChangeFileSize = vp_change_file_size,
    .FSGetFilePosition = vp_get_file_position,
    .FSGetFileSize = vp_get_file_size,
    .FSChangeLockMode = vp_change_lock_mode,
    .FSChangeFileMode = vp_change_file_mode,
    .FSSetDate = vp_set_date,
    .FSSetProtection = vp_set_protection,
    .FSSetComment = vp_set_comment,
    .FSSetGroup = vp_set_group,
    .FSSetUser = vp_set_user,
    .FSRename = vp_rename,
    .FSCreateSoftLink = vp_create_soft_link,
    .FSCreateHardLink = vp_create_hard_link,
    .FSReadSoftLink = vp_read_soft_link,
    .FSSameLock = vp_same_lock,
    .FSSameFile = vp_same_file,
    .FSFileSystemAttr = vp_filesystem_attr,
    .FSVolumeInfoData = vp_volume_info_data,
    .FSDeviceInfoData = vp_device_info_data,
    .FSReserved1 = NULL,
    .FSExamineObj = vp_examine_obj,
    .FSExamineLock = vp_examine_lock,
    .FSExamineFile = vp_examine_file,
    .FSExamineDir = vp_examine_dir,
    .FSInhibit = vp_inhibit,
    .FSWriteProtect = vp_write_protect,
    .FSFormat = vp_format,
    .FSSerialize = vp_serialize,
    .FSRelabel = vp_relabel,
    .FSReserved3 = NULL,
    .FSAddNotify = vp_add_notify,
    .FSRemoveNotify = vp_remove_notify,
    .FSLockRecord = vp_lock_record,
    .FSUnLockRecord = vp_unlock_record,
    .End_Marker = -1
};

const struct FileSystemVectors *odfs_os4_vector_template(void)
{
    return &odfs_os4_vectors;
}

struct FileSystemVectorPort *odfs_os4_alloc_vector_port(APTR fs_private)
{
    struct FileSystemVectorPort *vp;

    vp = AllocDosObjectTags(DOS_FSVECTORPORT,
                            ADO_Vectors,
                            (ULONG)&odfs_os4_vectors,
                            TAG_END);
    if (!vp)
        return NULL;

    vp->MP.mp_Node.ln_Type = NT_FILESYSTEM;
    vp->FSV.FSPrivate = fs_private;
    return vp;
}

void odfs_os4_free_vector_port(struct FileSystemVectorPort *vp)
{
    if (vp)
        FreeDosObject(DOS_FSVECTORPORT, vp);
}
