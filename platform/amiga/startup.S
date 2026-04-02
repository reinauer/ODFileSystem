/*
 * startup.S — AmigaDOS handler entry point
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * This is the resident entry point for the ODFileSystem handler.
 * It sets up the stack and calls the C entry point.
 */

	.text
	.globl	_start
	.globl	_entrypoint

_start:
_entrypoint:
	move.l	4.w,a6			/* SysBase */
	sub.l	a1,a1
	jsr	-0x126(a6)		/* FindTask(NULL) */
	move.l	d0,a4			/* a4 = our task */

	/* Check stack — we need at least 4096 bytes */
	move.l	62(a4),d0		/* tc_SPUpper */
	sub.l	58(a4),d0		/* tc_SPLower */
	cmp.l	#4096,d0
	bge.s	.stack_ok

	/* Stack too small — allocate a new one */
	move.l	#8192,d0
	move.l	#0x10001,d1		/* MEMF_PUBLIC|MEMF_CLEAR */
	jsr	-0xc6(a6)		/* AllocMem */
	tst.l	d0
	beq.s	.fail
	move.l	d0,a5			/* a5 = stack base */

	/* Set up StackSwap structure on current stack */
	sub.l	#12,sp
	move.l	a5,(sp)			/* stk_Lower */
	lea	8192(a5),a0
	move.l	a0,4(sp)		/* stk_Upper */
	move.l	a0,8(sp)		/* stk_Pointer (top of stack) */
	move.l	sp,a0
	jsr	-0x2dc(a6)		/* StackSwap */

	jsr	_handler_main		/* C entry point */

	/* Swap stack back */
	move.l	4.w,a6
	/* We can't easily swap back — just exit. The handler runs
	   until ACTION_DIE, at which point the process terminates. */
	bra.s	.done

.stack_ok:
	jsr	_handler_main		/* C entry point */

.done:
.fail:
	rts
