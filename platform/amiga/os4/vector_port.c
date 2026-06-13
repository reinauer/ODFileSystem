/*
 * vector_port.c - AmigaOS 4 filesystem vector-port frontend
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "vector_port.h"

#include "handler.h"

#include <dos/dos.h>
#include <dos/dostags.h>
#include <exec/lists.h>
#include <exec/nodes.h>
#include <utility/tagitem.h>

#include <proto/dos.h>
#include <proto/exec.h>

#include <stddef.h>
#include <string.h>

#ifndef ODFS_GIT_VERSION
#define ODFS_GIT_VERSION "unknown"
#endif

#define ODFS_OS4_FS_VERSION_NUMBER ((53UL << 16) | 4UL)
#define ODFS_OS4_MAX_FILE_SIZE     0x7fffffffffffffffULL

static handler_global_t *vp_global(struct FSVP *vp);

static void set_dos_error(int32 *res2, LONG err)
{
    if (res2)
        *res2 = err;
}

static handler_global_t *vp_require_global(struct FSVP *vp, int32 *res2)
{
    handler_global_t *g = vp_global(vp);

    if (!g)
        set_dos_error(res2, ERROR_OBJECT_WRONG_TYPE);
    return g;
}

static void set_unsupported(struct FSVP *vp, int32 *res2)
{
    handler_global_t *g = vp_require_global(vp, res2);

    if (!g)
        return;

    ODFS_TRACE(&g->log, ODFS_SUB_DOS,
               "unsupported vector hit -> ACTION_NOT_KNOWN");
    set_dos_error(res2, ERROR_ACTION_NOT_KNOWN);
}

static void set_write_protected(struct FSVP *vp, int32 *res2)
{
    handler_global_t *g = vp_require_global(vp, res2);

    if (!g)
        return;

    ODFS_TRACE(&g->log, ODFS_SUB_DOS, "write-protected vector hit");
    set_dos_error(res2, ERROR_DISK_WRITE_PROTECTED);
}

static handler_global_t *vp_global(struct FSVP *vp)
{
    return vp ? (handler_global_t *)vp->FSV.FSPrivate : NULL;
}

/*
 * Vector callbacks run in the calling process context, so every
 * callback that touches handler state must hold the filesystem
 * semaphore around the shared-operation call.
 */
static void fs_lock(handler_global_t *g)
{
    if (g)
        ObtainSemaphore(&g->fs_sem);
}

static void fs_unlock(handler_global_t *g)
{
    if (g)
        ReleaseSemaphore(&g->fs_sem);
}

static int32 return_dos_status(int32 *res2, LONG err)
{
    set_dos_error(res2, err);
    return err == 0 ? DOSTRUE : DOSFALSE;
}

static struct TagItem *next_filesystem_attr_tag(struct TagItem **taglist)
{
    struct TagItem *tag;

    if (!taglist)
        return NULL;

    tag = *taglist;
    while (tag) {
        switch (tag->ti_Tag) {
        case TAG_DONE:
            *taglist = NULL;
            return NULL;
        case TAG_IGNORE:
            tag++;
            break;
        case TAG_MORE:
            tag = (struct TagItem *)tag->ti_Data;
            break;
        case TAG_SKIP:
            tag += tag->ti_Data + 1;
            break;
        default:
            *taglist = tag + 1;
            return tag;
        }
    }

    *taglist = NULL;
    return NULL;
}

static uint32 filesystem_attr_version_buf_size(struct TagItem *taglist)
{
    struct TagItem *state = taglist;
    struct TagItem *tag;
    uint32 size = 0;

    while ((tag = next_filesystem_attr_tag(&state)) != NULL) {
        if (tag->ti_Tag == FSA_VersionStringR_BufSize)
            size = tag->ti_Data;
    }

    return size;
}

static void copy_filesystem_version_string(STRPTR buf, uint32 bufsize)
{
    static const char version[] = "ODFileSystem " ODFS_GIT_VERSION;
    uint32 i = 0;

    if (!buf || bufsize == 0)
        return;

    while (i + 1 < bufsize && version[i] != '\0') {
        buf[i] = version[i];
        i++;
    }
    buf[i] = '\0';
}

static odfs_lock_t *lock_from_vector(struct Lock *lock)
{
    return (odfs_lock_t *)LOCK_FROM_PTR(lock);
}

static odfs_fh_t *fh_from_vector(struct FileHandle *fh)
{
    return fh ? (odfs_fh_t *)fh->fh_Arg2 : NULL;
}

static const char *node_name_for_examine(handler_global_t *g,
                                         const odfs_node_t *node)
{
    if (g && node && odfs_node_matches_identity(node, &g->mount.root))
        return g->volname;
    return node ? node->name : "";
}

static struct ExamineData *take_stale_examine_data(
    struct PRIVATE_ExamineDirContext *ctx,
    size_t name_len,
    size_t comment_len)
{
    struct ExamineData *ed;

    if (!ctx)
        return NULL;

    while (!IsMinListEmpty(&ctx->StaleNodeList)) {
        ed = (struct ExamineData *)RemHead(
            (struct List *)&ctx->StaleNodeList);
        if (ed->NameSize >= name_len &&
            ed->CommentSize >= comment_len &&
            ed->LinkSize >= 1)
            return ed;

        FreeDosObject(DOS_EXAMINEDATA, ed);
    }

    return NULL;
}

static struct ExamineData *alloc_examine_data_from_context(
    handler_global_t *g,
    const odfs_node_t *node,
    struct PRIVATE_ExamineDirContext *ctx)
{
    const char *name;
    const char *comment = "";
    size_t name_len;
    size_t comment_len;
    struct ExamineData *ed;

    if (!node)
        return NULL;

    name = node_name_for_examine(g, node);
    if (node->amiga_as.has_comment)
        comment = node->amiga_as.comment;

    name_len = strlen(name) + 1u;
    comment_len = strlen(comment) + 1u;

    ed = take_stale_examine_data(ctx, name_len, comment_len);
    if (!ed) {
        /*
         * For the FSExamineDir() path the ExamineData entries must be
         * bound to the context's memory pool via ADO_ExamineDir_Context,
         * so DOS recycles and frees them through the context. For the
         * FSExamineObj() path ctx is NULL, which the tag treats as "not
         * specified" — the entry is then standalone and freed by the
         * caller with FreeDosObject().
         */
        ed = AllocDosObjectTags(DOS_EXAMINEDATA,
                                ADO_ExamineData_NameSize,
                                (ULONG)name_len,
                                ADO_ExamineData_CommentSize,
                                (ULONG)comment_len,
                                ADO_ExamineData_LinkSize,
                                1UL,
                                ADO_ExamineDir_Context,
                                (ULONG)ctx,
                                TAG_END);
    }
    if (!ed)
        return NULL;

    ed->EXDinfo = 0;
    ed->Type = (node->kind == ODFS_NODE_DIR) ?
        FSO_TYPE_DIRECTORY : FSO_TYPE_FILE;
    ed->FileSize = (node->kind == ODFS_NODE_DIR) ? -1LL : (int64)node->size;
    odfs_handler_node_date(node, &ed->Date);
    ed->RefCount = 0;
    ed->ObjectID = odfs_handler_node_key(node);
    ed->Protection = odfs_handler_node_protection(node);
    ed->OwnerUID = DOS_OWNER_NONE;
    ed->OwnerGID = DOS_OWNER_NONE;
    ed->FSPrivate = odfs_handler_node_key(node);

    if (ed->Name && ed->NameSize > 0) {
        strncpy(ed->Name, name, ed->NameSize - 1);
        ed->Name[ed->NameSize - 1] = '\0';
    }
    if (ed->Comment && ed->CommentSize > 0) {
        strncpy(ed->Comment, comment, ed->CommentSize - 1);
        ed->Comment[ed->CommentSize - 1] = '\0';
    }
    if (ed->Link && ed->LinkSize > 0)
        ed->Link[0] = '\0';

    return ed;
}

static struct ExamineData *alloc_examine_data(handler_global_t *g,
                                              const odfs_node_t *node)
{
    return alloc_examine_data_from_context(g, node, NULL);
}

static struct Lock *vp_lock(struct FSVP *vp,
                            int32 *res2,
                            struct Lock *rel_lock,
                            CONST_STRPTR obj,
                            int32 mode)
{
    handler_global_t *g = vp_require_global(vp, res2);
    odfs_lock_t *ol = NULL;
    LONG err;

    if (!g)
        return NULL;

    fs_lock(g);
    err = odfs_handler_lock_object(g, lock_from_vector(rel_lock),
                                   obj ? obj : "", mode, &ol);
    fs_unlock(g);
    ODFS_TRACE(&g->log, ODFS_SUB_DOS,
               "FSLock rel=%p obj='%s' mode=%ld -> ol=%p err=%ld",
               rel_lock, obj ? (const char *)obj : "", (long)mode, ol,
               (long)err);
    set_dos_error(res2, err);
    return (struct Lock *)LOCK_TO_PTR(ol);
}

static int32 vp_unlock(struct FSVP *vp, int32 *res2, struct Lock *lock)
{
    handler_global_t *g = vp_require_global(vp, res2);
    LONG err;

    if (!g)
        return DOSFALSE;

    fs_lock(g);
    err = odfs_handler_free_lock_object(g, lock_from_vector(lock));
    fs_unlock(g);
    return return_dos_status(res2, err);
}

static struct Lock *vp_dup_lock(struct FSVP *vp,
                                int32 *res2,
                                struct Lock *lock)
{
    handler_global_t *g = vp_require_global(vp, res2);
    odfs_lock_t *ol = NULL;
    LONG err;

    if (!g)
        return NULL;

    fs_lock(g);
    err = odfs_handler_dup_lock_object(g, lock_from_vector(lock), &ol);
    fs_unlock(g);
    ODFS_TRACE(&g->log, ODFS_SUB_DOS,
               "FSDupLock lock=%p -> ol=%p err=%ld", lock, ol, (long)err);
    set_dos_error(res2, err);
    return (struct Lock *)LOCK_TO_PTR(ol);
}

static struct Lock *vp_create_dir(struct FSVP *vp,
                                  int32 *res2,
                                  struct Lock *rel_lock,
                                  CONST_STRPTR obj)
{
    set_write_protected(vp, res2);
    (void)rel_lock;
    (void)obj;
    return NULL;
}

static struct Lock *vp_parent_dir(struct FSVP *vp,
                                  int32 *res2,
                                  struct Lock *dirlock)
{
    handler_global_t *g = vp_require_global(vp, res2);
    odfs_lock_t *parent = NULL;
    LONG err;

    if (!g)
        return NULL;

    fs_lock(g);
    err = odfs_handler_parent_lock_object(g, lock_from_vector(dirlock),
                                          &parent);
    fs_unlock(g);
    set_dos_error(res2, err);
    return (struct Lock *)LOCK_TO_PTR(parent);
}

static struct Lock *vp_dup_lock_from_fh(struct FSVP *vp,
                                        int32 *res2,
                                        struct FileHandle *filehandle)
{
    handler_global_t *g = vp_require_global(vp, res2);
    odfs_lock_t *ol = NULL;
    LONG err;

    if (!g)
        return NULL;

    fs_lock(g);
    err = odfs_handler_dup_lock_from_fh(g, fh_from_vector(filehandle), &ol);
    fs_unlock(g);
    set_dos_error(res2, err);
    return (struct Lock *)LOCK_TO_PTR(ol);
}

static int32 vp_open_from_lock(struct FSVP *vp,
                               int32 *res2,
                               struct FileHandle *file,
                               struct Lock *lock)
{
    handler_global_t *g = vp_require_global(vp, res2);
    odfs_fh_t *odfs_fh = NULL;
    LONG err;

    if (!g)
        return DOSFALSE;

    fs_lock(g);
    err = odfs_handler_open_from_lock_object(g, lock_from_vector(lock),
                                             &odfs_fh);
    fs_unlock(g);
    if (err == 0 && file) {
        file->fh_Arg1 = (BPTR)odfs_fh;
        file->fh_Arg2 = odfs_fh;
    }
    return return_dos_status(res2, err);
}

static struct Lock *vp_parent_of_fh(struct FSVP *vp,
                                    int32 *res2,
                                    struct FileHandle *file)
{
    handler_global_t *g = vp_require_global(vp, res2);
    odfs_lock_t *parent = NULL;
    LONG err;

    if (!g)
        return NULL;

    fs_lock(g);
    err = odfs_handler_parent_fh_object(g, fh_from_vector(file), &parent);
    fs_unlock(g);
    set_dos_error(res2, err);
    return (struct Lock *)LOCK_TO_PTR(parent);
}

static int32 vp_open(struct FSVP *vp,
                     int32 *res2,
                     struct FileHandle *fh,
                     struct Lock *rel_dir,
                     CONST_STRPTR obj,
                     int32 mode)
{
    handler_global_t *g = vp_require_global(vp, res2);
    odfs_fh_t *odfs_fh = NULL;
    LONG err;

    if (!g)
        return DOSFALSE;

    fs_lock(g);
    err = odfs_handler_open_object(g, lock_from_vector(rel_dir),
                                   obj ? obj : "", mode, &odfs_fh);
    fs_unlock(g);
    ODFS_TRACE(&g->log, ODFS_SUB_DOS,
               "FSOpen rel=%p obj='%s' mode=%ld -> err=%ld",
               rel_dir, obj ? (const char *)obj : "", (long)mode,
               (long)err);
    if (err == 0 && fh) {
        fh->fh_Arg1 = (BPTR)odfs_fh;
        fh->fh_Arg2 = odfs_fh;
    }
    return return_dos_status(res2, err);
}

static int32 vp_close(struct FSVP *vp, int32 *res2, struct FileHandle *file)
{
    handler_global_t *g = vp_require_global(vp, res2);
    LONG err;

    if (!g)
        return DOSFALSE;

    fs_lock(g);
    err = odfs_handler_close_object(g, fh_from_vector(file));
    fs_unlock(g);
    if (err == 0 && file) {
        file->fh_Arg1 = 0;
        file->fh_Arg2 = NULL;
    }
    return return_dos_status(res2, err);
}

static int32 vp_delete(struct FSVP *vp,
                       int32 *res2,
                       struct Lock *rel_dirlock,
                       CONST_STRPTR obj)
{
    set_write_protected(vp, res2);
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
    handler_global_t *g = vp_require_global(vp, res2);
    LONG actual = 0;
    LONG err;

    if (!g)
        return -1;

    fs_lock(g);
    err = odfs_handler_read_object(g, fh_from_vector(file),
                                   buffer, numbytes, &actual);
    fs_unlock(g);
    set_dos_error(res2, err);
    return err == 0 ? actual : -1;
}

static int32 vp_write(struct FSVP *vp,
                      int32 *res2,
                      struct FileHandle *file,
                      STRPTR buffer,
                      int32 numbytes)
{
    set_write_protected(vp, res2);
    (void)file;
    (void)buffer;
    (void)numbytes;
    return -1;
}

static int32 vp_flush(struct FSVP *vp, int32 *res2)
{
    handler_global_t *g = vp_require_global(vp, res2);

    if (!g)
        return DOSFALSE;

    set_dos_error(res2, 0);
    return DOSTRUE;
}

static int32 vp_change_file_position(struct FSVP *vp,
                                     int32 *res2,
                                     struct FileHandle *file,
                                     int32 mode,
                                     int64 position)
{
    handler_global_t *g = vp_require_global(vp, res2);
    int64_t oldpos;
    LONG err;

    if (!g)
        return DOSFALSE;

    fs_lock(g);
    err = odfs_handler_seek_object(g, fh_from_vector(file),
                                   position, mode, &oldpos);
    fs_unlock(g);
    return return_dos_status(res2, err);
}

static int32 vp_change_file_size(struct FSVP *vp,
                                 int32 *res2,
                                 struct FileHandle *file,
                                 int32 mode,
                                 int64 size)
{
    set_write_protected(vp, res2);
    (void)file;
    (void)mode;
    (void)size;
    return DOSFALSE;
}

static int64 vp_get_file_position(struct FSVP *vp,
                                  int32 *res2,
                                  struct FileHandle *file)
{
    handler_global_t *g = vp_require_global(vp, res2);
    int64_t pos;
    LONG err;

    if (!g)
        return -1;

    fs_lock(g);
    err = odfs_handler_get_file_position(g, fh_from_vector(file), &pos);
    fs_unlock(g);
    set_dos_error(res2, err);
    return err == 0 ? pos : -1;
}

static int64 vp_get_file_size(struct FSVP *vp,
                              int32 *res2,
                              struct FileHandle *file)
{
    handler_global_t *g = vp_require_global(vp, res2);
    int64_t size;
    LONG err;

    if (!g)
        return -1;

    fs_lock(g);
    err = odfs_handler_get_file_size(g, fh_from_vector(file), &size);
    fs_unlock(g);
    set_dos_error(res2, err);
    return err == 0 ? size : -1;
}

static int32 vp_change_lock_mode(struct FSVP *vp,
                                 int32 *res2,
                                 struct Lock *lock,
                                 int32 new_lock_mode)
{
    handler_global_t *g = vp_require_global(vp, res2);
    LONG err;

    if (!g)
        return DOSFALSE;

    fs_lock(g);
    err = odfs_handler_change_lock_mode(g, lock_from_vector(lock),
                                        new_lock_mode);
    fs_unlock(g);
    return return_dos_status(res2, err);
}

static int32 vp_change_file_mode(struct FSVP *vp,
                                 int32 *res2,
                                 struct FileHandle *fh,
                                 int32 new_lock_mode)
{
    handler_global_t *g = vp_require_global(vp, res2);
    LONG err;

    if (!g)
        return DOSFALSE;

    fs_lock(g);
    err = odfs_handler_change_file_mode(g, fh_from_vector(fh),
                                        new_lock_mode);
    fs_unlock(g);
    return return_dos_status(res2, err);
}

static int32 vp_set_date(struct FSVP *vp,
                         int32 *res2,
                         struct Lock *rel_dirlock,
                         CONST_STRPTR name,
                         const struct DateStamp *ds)
{
    set_write_protected(vp, res2);
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
    set_write_protected(vp, res2);
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
    set_write_protected(vp, res2);
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
    set_write_protected(vp, res2);
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
    set_write_protected(vp, res2);
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
    set_write_protected(vp, res2);
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
    set_write_protected(vp, res2);
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
    set_write_protected(vp, res2);
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
    handler_global_t *g = vp_require_global(vp, res2);
    LONG same = LOCK_DIFFERENT;
    LONG err;

    if (!g)
        return DOSFALSE;

    fs_lock(g);
    err = odfs_handler_same_lock_object(g,
                                        lock_from_vector(lock1),
                                        lock_from_vector(lock2),
                                        &same);
    fs_unlock(g);
    set_dos_error(res2, err);
    return (err == 0 && same == LOCK_SAME) ? DOSTRUE : DOSFALSE;
}

static int32 vp_same_file(struct FSVP *vp,
                          int32 *res2,
                          struct FileHandle *fh1,
                          struct FileHandle *fh2)
{
    handler_global_t *g = vp_require_global(vp, res2);
    LONG same = LOCK_DIFFERENT;
    LONG err;

    if (!g)
        return DOSFALSE;

    fs_lock(g);
    err = odfs_handler_same_file_object(g,
                                        fh_from_vector(fh1),
                                        fh_from_vector(fh2),
                                        &same);
    fs_unlock(g);
    set_dos_error(res2, err);
    return (err == 0 && same == FH_SAME) ? DOSTRUE : DOSFALSE;
}

static int32 vp_filesystem_attr(struct FSVP *vp,
                                int32 *res2,
                                struct TagItem *taglist)
{
    handler_global_t *g = vp_require_global(vp, res2);
    struct TagItem *state = taglist;
    struct TagItem *tag;
    uint32 version_buf_size;

    if (!g)
        return DOSFALSE;

    ODFS_TRACE(&g->log, ODFS_SUB_DOS, "FSFileSystemAttr taglist=%p",
               taglist);

    version_buf_size = filesystem_attr_version_buf_size(taglist);

    while ((tag = next_filesystem_attr_tag(&state)) != NULL) {
        switch (tag->ti_Tag) {
        case FSA_StringNameInput:
        case FSA_FileHandleInput:
        case FSA_LockInput:
        case FSA_MsgPortInput:
        case FSA_ShowRequesters:
        case FSA_VersionStringR_BufSize:
            break;

        case FSA_MaxFileNameLengthR:
            if (!tag->ti_Data)
                return return_dos_status(res2, ERROR_REQUIRED_ARG_MISSING);
            *(uint32 *)tag->ti_Data = MAX_VP_FILENAME;
            break;

        case FSA_VersionNumberR:
            if (!tag->ti_Data)
                return return_dos_status(res2, ERROR_REQUIRED_ARG_MISSING);
            *(uint32 *)tag->ti_Data = ODFS_OS4_FS_VERSION_NUMBER;
            break;

        case FSA_DOSTypeR:
            if (!tag->ti_Data)
                return return_dos_status(res2, ERROR_REQUIRED_ARG_MISSING);
            *(uint32 *)tag->ti_Data = ODFS_OS4_CD_DOSTYPE;
            break;

        case FSA_ActivityFlushTimeoutR:
        case FSA_InactivityFlushTimeoutR:
        case FSA_MaxRecycledEntriesR:
            if (!tag->ti_Data)
                return return_dos_status(res2, ERROR_REQUIRED_ARG_MISSING);
            *(uint32 *)tag->ti_Data = 0;
            break;

        case FSA_HasRecycledEntriesR:
            if (!tag->ti_Data)
                return return_dos_status(res2, ERROR_REQUIRED_ARG_MISSING);
            *(int32 *)tag->ti_Data = DOSFALSE;
            break;

        case FSA_VersionStringR:
            if (!tag->ti_Data || version_buf_size == 0)
                return return_dos_status(res2, ERROR_REQUIRED_ARG_MISSING);
            copy_filesystem_version_string((STRPTR)tag->ti_Data,
                                           version_buf_size);
            break;

        case FSA_MaxFileSizeR:
            if (!tag->ti_Data)
                return return_dos_status(res2, ERROR_REQUIRED_ARG_MISSING);
            *(uint64_t *)tag->ti_Data = ODFS_OS4_MAX_FILE_SIZE;
            break;

        case FSA_MaxFileNameLengthW:
        case FSA_ActivityFlushTimeoutW:
        case FSA_InactivityFlushTimeoutW:
        case FSA_MaxRecycledEntriesW:
            return return_dos_status(res2, ERROR_DISK_WRITE_PROTECTED);

        default:
            return return_dos_status(res2, ERROR_ACTION_NOT_KNOWN);
        }
    }

    return return_dos_status(res2, 0);
}

static int32 vp_volume_info_data(struct FSVP *vp,
                                 int32 *res2,
                                 struct InfoData *info)
{
    handler_global_t *g = vp_require_global(vp, res2);
    LONG err;

    if (!g)
        return DOSFALSE;

    fs_lock(g);
    err = odfs_handler_fill_info(g, NULL, info);
    fs_unlock(g);
    return return_dos_status(res2, err);
}

static int32 vp_device_info_data(struct FSVP *vp,
                                 int32 *res2,
                                 struct InfoData *info)
{
    handler_global_t *g = vp_require_global(vp, res2);
    LONG err;

    if (!g)
        return DOSFALSE;

    fs_lock(g);
    err = odfs_handler_fill_info(g, NULL, info);
    fs_unlock(g);
    return return_dos_status(res2, err);
}

static struct ExamineData *vp_examine_obj(struct FSVP *vp,
                                          int32 *res2,
                                          struct Lock *lock,
                                          CONST_STRPTR object)
{
    handler_global_t *g = vp_require_global(vp, res2);
    odfs_lock_t *ol = NULL;
    struct ExamineData *ed;
    LONG err;

    if (!g)
        return NULL;

    fs_lock(g);
    err = odfs_handler_lock_object(g, lock_from_vector(lock),
                                   object ? object : "", SHARED_LOCK, &ol);
    ODFS_TRACE(&g->log, ODFS_SUB_DOS,
               "FSExamineObj lock=%p obj='%s' -> err=%ld",
               lock, object ? (const char *)object : "", (long)err);
    if (err != 0) {
        fs_unlock(g);
        set_dos_error(res2, err);
        return NULL;
    }

    ed = alloc_examine_data(g, ol->entry ? &ol->entry->fnode : NULL);
    (void)odfs_handler_free_lock_object(g, ol);
    fs_unlock(g);
    set_dos_error(res2, ed ? 0 : ERROR_NO_FREE_STORE);
    return ed;
}

static struct ExamineData *vp_examine_lock(struct FSVP *vp,
                                           int32 *res2,
                                           struct Lock *lock)
{
    handler_global_t *g = vp_require_global(vp, res2);
    const odfs_node_t *node = NULL;
    struct ExamineData *ed;
    LONG err;

    if (!g)
        return NULL;

    fs_lock(g);
    err = odfs_handler_get_lock_node(g, lock_from_vector(lock), &node);
    ODFS_TRACE(&g->log, ODFS_SUB_DOS,
               "FSExamineLock lock=%p -> err=%ld name='%s'",
               lock, (long)err, (err == 0 && node) ? node->name : "");
    if (err != 0) {
        fs_unlock(g);
        set_dos_error(res2, err);
        return NULL;
    }

    ed = alloc_examine_data(g, node);
    fs_unlock(g);
    set_dos_error(res2, ed ? 0 : ERROR_NO_FREE_STORE);
    return ed;
}

static struct ExamineData *vp_examine_file(struct FSVP *vp,
                                           int32 *res2,
                                           struct FileHandle *file)
{
    handler_global_t *g = vp_require_global(vp, res2);
    const odfs_node_t *node = NULL;
    struct ExamineData *ed;
    LONG err;

    if (!g)
        return NULL;

    fs_lock(g);
    err = odfs_handler_get_fh_node(g, fh_from_vector(file), &node);
    if (err != 0) {
        fs_unlock(g);
        set_dos_error(res2, err);
        return NULL;
    }

    ed = alloc_examine_data(g, node);
    fs_unlock(g);
    set_dos_error(res2, ed ? 0 : ERROR_NO_FREE_STORE);
    return ed;
}

static int32 vp_examine_dir(struct FSVP *vp,
                            int32 *res2,
                            struct PRIVATE_ExamineDirContext *ctx)
{
    handler_global_t *g = vp_require_global(vp, res2);
    odfs_node_t entry;
    ULONG key = 0;
    struct ExamineData *ed;
    LONG err;

    if (!g)
        return DOSFALSE;

    if (!ctx)
        return return_dos_status(res2, ERROR_REQUIRED_ARG_MISSING);

    fs_lock(g);
    err = odfs_handler_next_dir_entry(g, lock_from_vector(ctx->ReferenceLock),
                                      ctx->FSPrivate[0], &entry, &key);
    ODFS_TRACE(&g->log, ODFS_SUB_DOS,
               "FSExamineDir ctx=%p refLock=%p prevKey=%lx -> "
               "err=%ld key=%lx name='%s'",
               ctx, ctx->ReferenceLock,
               (unsigned long)ctx->FSPrivate[0],
               (long)err, (unsigned long)key,
               (err == 0) ? entry.name : "");
    if (err != 0) {
        fs_unlock(g);
        return return_dos_status(res2, err);
    }

    ed = alloc_examine_data_from_context(g, &entry, ctx);
    fs_unlock(g);
    if (!ed)
        return return_dos_status(res2, ERROR_NO_FREE_STORE);

    AddTail((struct List *)&ctx->FreshNodeList, (struct Node *)&ed->EXDnode);
    ctx->FSPrivate[0] = key;
    return return_dos_status(res2, 0);
}

static int32 vp_inhibit(struct FSVP *vp, int32 *res2, int32 inhibit_state)
{
    handler_global_t *g = vp_require_global(vp, res2);
    LONG err;

    if (!g)
        return DOSFALSE;

    fs_lock(g);
    err = odfs_handler_inhibit(g, inhibit_state);
    fs_unlock(g);
    return return_dos_status(res2, err);
}

static int32 vp_write_protect(struct FSVP *vp,
                              int32 *res2,
                              int32 wp_state,
                              uint32 passkey)
{
    set_write_protected(vp, res2);
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
    set_write_protected(vp, res2);
    (void)new_volname;
    (void)dostype;
    (void)spare;
    return DOSFALSE;
}

static int32 vp_serialize(struct FSVP *vp, int32 *res2)
{
    handler_global_t *g = vp_require_global(vp, res2);

    if (!g)
        return DOSFALSE;

    set_dos_error(res2, 0);
    return DOSTRUE;
}

static int32 vp_relabel(struct FSVP *vp,
                        int32 *res2,
                        CONST_STRPTR new_volumename)
{
    set_write_protected(vp, res2);
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

void odfs_os4_emulate_packet(struct FileSystemVectorPort *vp,
                             struct DosPacket *pkt)
{
    if (!pkt)
        return;

    if (vp && vp->FSV.DOSEmulatePacket) {
        vp->FSV.DOSEmulatePacket(vp, pkt);
        return;
    }

    pkt->dp_Res1 = DOSFALSE;
    pkt->dp_Res2 = ERROR_ACTION_NOT_KNOWN;
}

void odfs_os4_invalidate_vector_port(struct FileSystemVectorPort *vp)
{
    if (vp)
        vp->FSV.Version = 0;
}
