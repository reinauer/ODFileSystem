/*
 * Exercise the production OS4 vector guard with controlled task switches.
 * The Exec stubs model exclusive semaphore ownership and nesting; hooks
 * schedule shutdown while a vector is admitted or waiting for its lock.
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "test_harness.h"
#include "../../platform/amiga/os4/vector_guard.h"

#include <assert.h>

static handler_global_t handler;
static struct FileSystemVectorPort port;
static int current_task;
static int forbidden;
static int shutdown_result;
static void (*on_permit)(void);
static void (*on_wait)(void);

void Forbid(void)
{
    forbidden++;
}

void Permit(void)
{
    void (*hook)(void) = on_permit;

    assert(forbidden > 0);
    forbidden--;
    if (!forbidden && hook) {
        on_permit = NULL;
        hook();
    }
}

int AttemptSemaphore(struct SignalSemaphore *sem)
{
    assert(forbidden > 0);
    if (sem->owner && sem->owner != current_task)
        return 0;
    sem->owner = current_task;
    sem->depth++;
    return 1;
}

void ObtainSemaphore(struct SignalSemaphore *sem)
{
    assert(forbidden > 0);
    if (sem->owner && sem->owner != current_task) {
        /* Exec Wait temporarily permits scheduling, restoring Forbid
         * when the waiter wakes with ownership of the semaphore. */
        int saved_forbidden = forbidden;
        assert(on_wait);
        forbidden = 0;
        on_wait();
        forbidden = saved_forbidden;
    }
    assert(AttemptSemaphore(sem));
}

void ReleaseSemaphore(struct SignalSemaphore *sem)
{
    assert(sem->owner == current_task && sem->depth > 0);
    if (--sem->depth == 0)
        sem->owner = 0;
}

static void init_handler(void)
{
    memset(&handler, 0, sizeof(handler));
    memset(&port, 0, sizeof(port));
    handler.locklist.mlh_TailPred =
        (struct MinNode *)&handler.locklist.mlh_Head;
    handler.fhlist.mlh_TailPred =
        (struct MinNode *)&handler.fhlist.mlh_Head;
    handler.vector_port = &port;
    port.FSV.Version = 53;
    port.FSV.FSPrivate = &handler;
    current_task = 1;
    forbidden = 0;
    shutdown_result = -1;
    on_permit = NULL;
    on_wait = NULL;
}

static void try_shutdown_from_handler(void)
{
    int caller = current_task;
    current_task = 2;
    shutdown_result = odfs_os4_begin_shutdown(&handler);
    current_task = caller;
}

static void finish_owner_after_shutdown_attempt(void)
{
    int caller = current_task;
    try_shutdown_from_handler();
    current_task = handler.fs_sem.owner;
    ReleaseSemaphore(&handler.fs_sem);
    current_task = caller;
}

TEST(active_callback_refuses_shutdown_before_creating_objects)
{
    init_handler();
    on_permit = try_shutdown_from_handler;
    ASSERT(odfs_os4_acquire_handler(&port) == &handler);
    ASSERT_EQ(shutdown_result, 0);
    ASSERT(port.FSV.FSPrivate == &handler);
    ASSERT_EQ(port.FSV.Version, 53);
    ReleaseSemaphore(&handler.fs_sem);
    ASSERT(odfs_os4_begin_shutdown(&handler));
    ASSERT_EQ(forbidden, 0);
}

TEST(waiting_callback_refuses_shutdown)
{
    init_handler();
    handler.fs_sem.owner = 3;
    handler.fs_sem.depth = 1;
    on_wait = finish_owner_after_shutdown_attempt;
    ASSERT(odfs_os4_acquire_handler(&port) == &handler);
    ASSERT_EQ(shutdown_result, 0);
    ASSERT_EQ(handler.fs_sem.owner, current_task);
    ReleaseSemaphore(&handler.fs_sem);
    ASSERT(odfs_os4_begin_shutdown(&handler));
    ASSERT_EQ(forbidden, 0);
}

TEST(outstanding_objects_keep_vector_entry_open)
{
    struct MinNode object;
    init_handler();
    handler.locklist.mlh_TailPred = &object;
    ASSERT_EQ(odfs_os4_begin_shutdown(&handler), 0);
    ASSERT(port.FSV.FSPrivate == &handler);
    handler.locklist.mlh_TailPred =
        (struct MinNode *)&handler.locklist.mlh_Head;
    handler.fhlist.mlh_TailPred = &object;
    ASSERT_EQ(odfs_os4_begin_shutdown(&handler), 0);
    ASSERT_EQ(handler.fs_sem.depth, 0);
    ASSERT_EQ(port.FSV.Version, 53);
    ASSERT_EQ(forbidden, 0);
}

static void enter_after_shutdown(void)
{
    assert(port.FSV.Version == 0);
    assert(odfs_os4_acquire_handler(&port) == NULL);
}

TEST(idle_shutdown_closes_entry_before_allowing_task_switches)
{
    init_handler();
    on_permit = enter_after_shutdown;
    ASSERT(odfs_os4_begin_shutdown(&handler));
    ASSERT(port.FSV.FSPrivate == NULL);
    ASSERT(odfs_os4_acquire_handler(&port) == NULL);
    ASSERT(odfs_os4_acquire_handler(NULL) == NULL);
    ASSERT_EQ(handler.fs_sem.depth, 0);
    ASSERT_EQ(forbidden, 0);
}

TEST_MAIN()
