/* Minimal host-side types for testing the OS4 vector guard. */
/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef ODFS_TEST_OS4_HANDLER_H
#define ODFS_TEST_OS4_HANDLER_H

#include <stddef.h>

struct SignalSemaphore {
    int owner;
    unsigned int depth;
};

struct MinNode {
    struct MinNode *mln_Succ;
    struct MinNode *mln_Pred;
};

struct MinList {
    struct MinNode *mlh_Head;
    struct MinNode *mlh_Tail;
    struct MinNode *mlh_TailPred;
};

struct FileSystemVectorPort {
    struct {
        unsigned int Version;
        void *FSPrivate;
    } FSV;
};

typedef struct handler_global {
    struct SignalSemaphore fs_sem;
    struct MinList locklist;
    struct MinList fhlist;
    struct FileSystemVectorPort *vector_port;
} handler_global_t;

#endif
