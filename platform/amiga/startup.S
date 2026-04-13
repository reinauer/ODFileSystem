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

/* The NDK ships its asm headers (exec/tasks.i, lvo/exec_lib.i) in
 * Amiga assembler syntax (IFND, EQU, ...), which GNU as does not
 * understand. We define the few constants we need here.
 */

/* Exec library vector offsets (from NDK lvo/exec_lib.i) */
#define _LVOFindTask	-294
#define _LVOAllocMem	-198
#define _LVOFreeMem	-210
#define _LVOStackSwap	-732

/* struct Task offsets (from NDK exec/tasks.i) */
#define TC_SPLOWER	58
#define TC_SPUPPER	62

/* struct StackSwapStruct offsets (from NDK exec/tasks.i) */
#define STK_LOWER	0
#define STK_UPPER	4
#define STK_POINTER	8
#define STK_SIZEOF	12

/* AllocMem flags (from NDK exec/memory.i) */
#define MEMF_PUBLIC	0x00000001
#define MEMF_CLEAR	0x00010000

#define MIN_STACK	4096
#define SWAP_STACK	8192

_start:
_entrypoint:
	move.l	4.w,a6			/* SysBase */
	sub.l	a1,a1
	jsr	_LVOFindTask(a6)	/* FindTask(NULL) */
	move.l	d0,a4			/* a4 = our task */

	/* Check stack — swap to a larger one if too small */
	move.l	TC_SPUPPER(a4),d0
	sub.l	TC_SPLOWER(a4),d0
	cmp.l	#MIN_STACK,d0
	bge.s	.stack_ok

	/* Stack too small — allocate a new one */
	move.l	#SWAP_STACK,d0
	move.l	#MEMF_PUBLIC+MEMF_CLEAR,d1
	jsr	_LVOAllocMem(a6)
	tst.l	d0
	beq.s	.fail
	move.l	d0,a5			/* a5 = stack base */

	/* Set up StackSwap structure on current stack */
	sub.l	#STK_SIZEOF,sp
	move.l	a5,STK_LOWER(sp)
	lea	SWAP_STACK(a5),a0
	move.l	a0,STK_UPPER(sp)
	move.l	a0,STK_POINTER(sp)	/* initial SP = top of new stack */
	move.l	sp,a2			/* save struct addr (callee-saved) */
	move.l	a2,a0
	jsr	_LVOStackSwap(a6)

	jsr	_handler_main		/* C entry point */

	/* Swap back to original stack (struct still on old stack via a2) */
	move.l	4.w,a6
	move.l	a2,a0
	jsr	_LVOStackSwap(a6)

	/* Free the allocated stack */
	move.l	a5,a1
	move.l	#SWAP_STACK,d0
	jsr	_LVOFreeMem(a6)

	lea	STK_SIZEOF(sp),sp	/* pop StackSwap struct */
	bra.s	.done

.stack_ok:
	jsr	_handler_main		/* C entry point */

.done:
.fail:
	rts
