/* Host-side scheduling hooks used by the real OS4 vector guard. */
/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef ODFS_TEST_OS4_EXEC_H
#define ODFS_TEST_OS4_EXEC_H

struct SignalSemaphore;
void Forbid(void);
void Permit(void);
void ObtainSemaphore(struct SignalSemaphore *sem);
int AttemptSemaphore(struct SignalSemaphore *sem);
void ReleaseSemaphore(struct SignalSemaphore *sem);

#endif
