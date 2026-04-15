/*
 * test_handler.c — AmigaDOS smoke tests for handler path semantics
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/dos.h>

#include <stdlib.h>
#include <string.h>

static void print_fault(const char *what)
{
    UBYTE buf[160];

    if (!Fault(IoErr(), (CONST_STRPTR)what, (STRPTR)buf, sizeof(buf)))
        strcpy((char *)buf, "unknown DOS error");
    PutStr((CONST_STRPTR)buf);
    PutStr((CONST_STRPTR)"\n");
}

static int print_lock_path(const char *label, BPTR lock)
{
    UBYTE buf[512];

    if (!NameFromLock(lock, (STRPTR)buf, sizeof(buf))) {
        Printf((CONST_STRPTR)"%s<error> ", (LONG)label);
        print_fault("NameFromLock");
        return 0;
    }

    Printf((CONST_STRPTR)"%s%s\n", (LONG)label, (LONG)buf);
    return 1;
}

static int print_bstr(const char *label, BSTR bstr)
{
    UBYTE buf[160];
    UBYTE *raw;
    int len;

    if (!bstr) {
        Printf((CONST_STRPTR)"%s<none>\n", (LONG)label);
        return 1;
    }

    raw = (UBYTE *)BADDR(bstr);
    len = raw[0];
    if (len >= (int)sizeof(buf))
        len = (int)sizeof(buf) - 1;
    memcpy(buf, raw + 1, (size_t)len);
    buf[len] = '\0';

    Printf((CONST_STRPTR)"%s%s\n", (LONG)label, (LONG)buf);
    return 1;
}

static int print_fib_name(const char *label, const struct FileInfoBlock *fib)
{
    UBYTE buf[160];
    int len;

    len = fib->fib_FileName[0];
    if (len >= (int)sizeof(buf))
        len = (int)sizeof(buf) - 1;
    memcpy(buf, &fib->fib_FileName[1], (size_t)len);
    buf[len] = '\0';

    Printf((CONST_STRPTR)"%s%s\n", (LONG)label, (LONG)buf);
    return 1;
}

static int print_lock_metadata(BPTR lock)
{
    struct InfoData info;
    struct FileInfoBlock fib;
    struct DeviceList *volnode;

    memset(&info, 0, sizeof(info));
    if (!Info(lock, &info)) {
        PutStr((CONST_STRPTR)"INFO:   <error> ");
        print_fault("Info");
        return 0;
    }

    volnode = info.id_VolumeNode
        ? (struct DeviceList *)BADDR(info.id_VolumeNode)
        : NULL;
    if (!print_bstr("INFO:   ", volnode ? volnode->dl_Name : 0))
        return 0;

    memset(&fib, 0, sizeof(fib));
    if (!Examine(lock, &fib)) {
        PutStr((CONST_STRPTR)"EXAM:   <error> ");
        print_fault("Examine");
        return 0;
    }

    return print_fib_name("EXAM:   ", &fib);
}

static int print_fh_path(const char *label, BPTR fh)
{
    UBYTE buf[512];

    if (!NameFromFH(fh, (STRPTR)buf, sizeof(buf))) {
        Printf((CONST_STRPTR)"%s<error> ", (LONG)label);
        print_fault("NameFromFH");
        return 0;
    }

    Printf((CONST_STRPTR)"%s%s\n", (LONG)label, (LONG)buf);
    return 1;
}

static LONG run_parent_chain(BPTR start_lock, LONG up_limit)
{
    BPTR cur;
    LONG steps = 0;

    cur = DupLock(start_lock);
    if (!cur) {
        print_fault("DupLock");
        return RETURN_FAIL;
    }

    while (cur && (up_limit < 0 || steps < up_limit)) {
        BPTR next = ParentDir(cur);

        cur = next;
        if (!cur)
            break;

        steps++;
        if (!print_lock_path("PARENT: ", cur)) {
            UnLock(cur);
            return RETURN_FAIL;
        }
    }

    if (cur)
        UnLock(cur);
    return RETURN_OK;
}

static LONG run_relative_lock(BPTR start_lock, const char *rel)
{
    BPTR temp;
    BPTR old;
    BPTR restore;
    BPTR rel_lock;

    temp = DupLock(start_lock);
    if (!temp) {
        print_fault("DupLock");
        return RETURN_FAIL;
    }

    old = CurrentDir(temp);
    rel_lock = Lock((CONST_STRPTR)rel, SHARED_LOCK);
    restore = CurrentDir(old);
    if (restore)
        UnLock(restore);

    if (!rel_lock) {
        Printf((CONST_STRPTR)"REL=%s failed: ", (LONG)rel);
        print_fault("Lock");
        return RETURN_FAIL;
    }

    print_lock_path("REL:    ", rel_lock);
    UnLock(rel_lock);
    return RETURN_OK;
}

static LONG run_parent_of_fh(const char *path)
{
    BPTR fh;
    BPTR parent;

    fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    if (!fh) {
        Printf((CONST_STRPTR)"FILE=%s open failed: ", (LONG)path);
        print_fault("Open");
        return RETURN_FAIL;
    }

    if (!print_fh_path("FH:     ", fh)) {
        Close(fh);
        return RETURN_FAIL;
    }

    parent = ParentOfFH(fh);
    if (!parent) {
        PutStr((CONST_STRPTR)"ParentOfFH failed: ");
        print_fault("ParentOfFH");
        Close(fh);
        return RETURN_FAIL;
    }

    print_lock_path("FHPAR:  ", parent);
    UnLock(parent);
    Close(fh);
    return RETURN_OK;
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    const char *rel = NULL;
    const char *file = NULL;
    LONG up_limit = -1;
    LONG rc = RETURN_OK;
    BPTR lock;
    int i;

    for (i = 1; i < argc; i++) {
        if (strncmp(argv[i], "UP=", 3) == 0) {
            up_limit = strtol(argv[i] + 3, NULL, 10);
        } else if (strncmp(argv[i], "REL=", 4) == 0) {
            rel = argv[i] + 4;
        } else if (strncmp(argv[i], "FILE=", 5) == 0) {
            file = argv[i] + 5;
        } else if (!path) {
            path = argv[i];
        } else {
            PutStr((CONST_STRPTR)
                   "Usage: test_handler PATH [UP=n] [REL=path] [FILE=path]\n");
            return RETURN_FAIL;
        }
    }

    if (!path) {
        PutStr((CONST_STRPTR)
               "Usage: test_handler PATH [UP=n] [REL=path] [FILE=path]\n");
        return RETURN_FAIL;
    }

    lock = Lock((CONST_STRPTR)path, SHARED_LOCK);
    if (!lock) {
        Printf((CONST_STRPTR)"PATH=%s lock failed: ", (LONG)path);
        print_fault("Lock");
        return RETURN_FAIL;
    }

    if (!print_lock_path("LOCK:   ", lock))
        rc = RETURN_FAIL;
    if (rc == RETURN_OK && !print_lock_metadata(lock))
        rc = RETURN_FAIL;

    if (rc == RETURN_OK && run_parent_chain(lock, up_limit) != RETURN_OK)
        rc = RETURN_FAIL;

    if (rc == RETURN_OK && rel && run_relative_lock(lock, rel) != RETURN_OK)
        rc = RETURN_FAIL;

    UnLock(lock);

    if (rc == RETURN_OK && file && run_parent_of_fh(file) != RETURN_OK)
        rc = RETURN_FAIL;

    return rc;
}
