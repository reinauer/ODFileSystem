/*
 * string.S - memory and string functions for the AmigaOS 3 handler
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The handler links without a C library, so it provides the functions
 * that the compiler and the core library call. All of them take their
 * arguments on the stack, as declared in <string.h>.
 *
 * memcpy is tuned for the handler's calls, which are mostly copies of
 * up to 16 bytes and 2 KiB sector copies:
 *  - up to 16 bytes: unrolled byte moves, entered n moves before the end
 *  - when source and destination share their alignment: long words up
 *    to COPYMEM_MIN bytes, exec's CopyMem from there
 *  - when they do not: long words on a 68020 or later, which allows
 *    unaligned accesses, and bytes on a 68000
 */

#define _LVOCopyMem	-624
#define AttnFlags	296		/* struct ExecBase */
#define AFB_68020	1

/* Below this, the CopyMem call costs more than it saves. */
#define COPYMEM_MIN	160

	.text

/* void *memcpy(void *dst, const void *src, size_t n) */
	.globl	_memcpy
	.even
_memcpy:
	move.l	12(sp),d0
	move.l	8(sp),a0
	move.l	4(sp),a1
.Lcopy:
	moveq	#16,d1
	cmp.l	d1,d0
	bls	.Lsmall
	cmp.l	#COPYMEM_MIN,d0
	bcc	.Lbig

/* n > 16, forward, without CopyMem. memmove enters here too. */
.Lmid:
	move.w	a0,d1
	add.w	a1,d1
	lsr.b	#1,d1
	bcs.s	.Lunaligned
	move.w	a0,d1
	lsr.b	#1,d1
	bcc.s	.Llongs
	move.b	(a0)+,(a1)+
	subq.l	#1,d0
/* n >= 16: copy n & 4 and n & 8, then blocks of 16, then the rest */
.Llongs:
	move.l	d0,d1
	lsr.l	#3,d1
	bcc.s	1f
	move.l	(a0)+,(a1)+
1:	lsr.l	#1,d1
	bcc.s	1f
	move.l	(a0)+,(a1)+
	move.l	(a0)+,(a1)+
1:	bra.s	3f
2:	move.l	(a0)+,(a1)+
	move.l	(a0)+,(a1)+
	move.l	(a0)+,(a1)+
	move.l	(a0)+,(a1)+
3:	dbra	d1,2b
	sub.l	#0x10000,d1
	bcc.s	2b
	btst	#1,d0
	beq.s	1f
	move.w	(a0)+,(a1)+
1:	btst	#0,d0
	beq.s	1f
	move.b	(a0)+,(a1)+
1:	move.l	4(sp),d0
	rts

/* Source and destination differ in alignment; n > 16. */
.Lunaligned:
#ifndef __mc68020__
	move.l	4.w,d1
	exg	d1,a0
	btst	#AFB_68020,AttnFlags+1(a0)
	exg	d1,a0
	bne.s	.Llongs
	move.l	d0,d1
	lsr.l	#4,d1
	bra.s	3f
2:	move.b	(a0)+,(a1)+
	move.b	(a0)+,(a1)+
	move.b	(a0)+,(a1)+
	move.b	(a0)+,(a1)+
	move.b	(a0)+,(a1)+
	move.b	(a0)+,(a1)+
	move.b	(a0)+,(a1)+
	move.b	(a0)+,(a1)+
	move.b	(a0)+,(a1)+
	move.b	(a0)+,(a1)+
	move.b	(a0)+,(a1)+
	move.b	(a0)+,(a1)+
	move.b	(a0)+,(a1)+
	move.b	(a0)+,(a1)+
	move.b	(a0)+,(a1)+
	move.b	(a0)+,(a1)+
3:	dbra	d1,2b
	sub.l	#0x10000,d1
	bcc.s	2b
	and.w	#15,d0
	/* fall through for the remaining 0-15 bytes */
#else
	bra.s	.Llongs
#endif

/* n <= 16 */
.Lsmall:
	add.w	d0,d0
	neg.w	d0
	jmp	.Lsmall_end(pc,d0.w)
	move.b	(a0)+,(a1)+
	move.b	(a0)+,(a1)+
	move.b	(a0)+,(a1)+
	move.b	(a0)+,(a1)+
	move.b	(a0)+,(a1)+
	move.b	(a0)+,(a1)+
	move.b	(a0)+,(a1)+
	move.b	(a0)+,(a1)+
	move.b	(a0)+,(a1)+
	move.b	(a0)+,(a1)+
	move.b	(a0)+,(a1)+
	move.b	(a0)+,(a1)+
	move.b	(a0)+,(a1)+
	move.b	(a0)+,(a1)+
	move.b	(a0)+,(a1)+
	move.b	(a0)+,(a1)+
.Lsmall_end:
	move.l	4(sp),d0
	rts

/* n >= COPYMEM_MIN */
.Lbig:
	move.w	a0,d1
	add.w	a1,d1
	lsr.b	#1,d1
	bcs.s	.Lunaligned
	move.l	a6,-(sp)
	move.l	4.w,a6
	jsr	_LVOCopyMem(a6)
	move.l	(sp)+,a6
	move.l	4(sp),d0
	rts

/* void *memmove(void *dst, const void *src, size_t n) */
	.globl	_memmove
	.even
_memmove:
	move.l	12(sp),d0
	move.l	8(sp),a0
	move.l	4(sp),a1
	move.l	a1,d1
	sub.l	a0,d1
	beq.s	2f
	cmp.l	d0,d1
	bcs.s	.Lmm_back
	/* CopyMem does not support overlap: hand memcpy only disjoint
	 * regions, and copy an overlap with dst below src forward here. */
	move.l	a0,d1
	sub.l	a1,d1
	cmp.l	d0,d1
	bcc	.Lcopy
	moveq	#16,d1
	cmp.l	d1,d0
	bls	.Lsmall
	bra	.Lmid
/* dst overlaps the end of src: copy backward */
.Lmm_back:
	add.l	d0,a0
	add.l	d0,a1
1:	move.b	-(a0),-(a1)
	subq.l	#1,d0
	bne.s	1b
2:	move.l	4(sp),d0
	rts

/* void *memset(void *dst, int c, size_t n) */
	.globl	_memset
	.even
_memset:
	move.l	12(sp),d0
	move.l	4(sp),a0
	move.b	11(sp),d1
	cmp.l	#16,d0
	bhi.s	.Lset_mid
	add.w	d0,d0
	neg.w	d0
	jmp	.Lset_end(pc,d0.w)
	move.b	d1,(a0)+
	move.b	d1,(a0)+
	move.b	d1,(a0)+
	move.b	d1,(a0)+
	move.b	d1,(a0)+
	move.b	d1,(a0)+
	move.b	d1,(a0)+
	move.b	d1,(a0)+
	move.b	d1,(a0)+
	move.b	d1,(a0)+
	move.b	d1,(a0)+
	move.b	d1,(a0)+
	move.b	d1,(a0)+
	move.b	d1,(a0)+
	move.b	d1,(a0)+
	move.b	d1,(a0)+
.Lset_end:
	move.l	4(sp),d0
	rts
/* n > 16 */
.Lset_mid:
	move.l	d2,-(sp)
	move.w	a0,d2
	lsr.b	#1,d2
	bcc.s	1f
	move.b	d1,(a0)+
	subq.l	#1,d0
1:	move.b	d1,d2
	lsl.w	#8,d1
	move.b	d2,d1
	move.w	d1,d2
	swap	d1
	move.w	d2,d1
	move.l	d0,d2
	lsr.l	#3,d2
	bcc.s	1f
	move.l	d1,(a0)+
1:	lsr.l	#1,d2
	bcc.s	1f
	move.l	d1,(a0)+
	move.l	d1,(a0)+
1:	bra.s	3f
2:	move.l	d1,(a0)+
	move.l	d1,(a0)+
	move.l	d1,(a0)+
	move.l	d1,(a0)+
3:	dbra	d2,2b
	sub.l	#0x10000,d2
	bcc.s	2b
	btst	#1,d0
	beq.s	1f
	move.w	d1,(a0)+
1:	btst	#0,d0
	beq.s	1f
	move.b	d1,(a0)+
1:	move.l	(sp)+,d2
	move.l	4(sp),d0
	rts

/* char *strchr(const char *s, int c) */
	.globl	_strchr
	.even
_strchr:
	move.l	4(sp),a0
	move.b	11(sp),d1
1:	move.b	(a0)+,d0
	beq.s	3f
	cmp.b	d0,d1
	beq.s	2f
	move.b	(a0)+,d0
	beq.s	3f
	cmp.b	d0,d1
	bne.s	1b
2:	subq.l	#1,a0
	move.l	a0,d0
	rts
	/* end of string: found only when looking for the terminator */
3:	tst.b	d1
	beq.s	2b
	moveq	#0,d0
	rts

/* int strcmp(const char *a, const char *b) */
	.globl	_strcmp
	.even
_strcmp:
	move.l	4(sp),a0
	move.l	8(sp),a1
	/* Compare long words while both strings are long-aligned. An
	 * aligned long word never crosses a page, so reading past the
	 * terminator within it is safe. */
	move.w	a0,d0
	move.w	a1,d1
	or.w	d1,d0
	and.w	#3,d0
	bne.s	.Lcmp_bytes
1:	move.l	(a0),d0
	cmp.l	(a1),d0
	bne.s	.Lcmp_bytes
	move.l	d0,d1
	sub.l	#0x01010101,d1
	not.l	d0
	and.l	d0,d1
	and.l	#0x80808080,d1
	bne.s	.Lcmp_equal
	addq.l	#4,a0
	addq.l	#4,a1
	bra.s	1b
.Lcmp_bytes:
	move.b	(a0)+,d0
	beq.s	.Lcmp_a_end
	cmp.b	(a1)+,d0
	bne.s	.Lcmp_diff
	move.b	(a0)+,d0
	beq.s	.Lcmp_a_end
	cmp.b	(a1)+,d0
	beq.s	.Lcmp_bytes
	/* unsigned char difference: only the sign matters */
.Lcmp_diff:
	bhi.s	1f
	moveq	#-1,d0
	rts
1:	moveq	#1,d0
	rts
.Lcmp_a_end:
	tst.b	(a1)
	bne.s	1f
.Lcmp_equal:
	moveq	#0,d0
	rts
1:	moveq	#-1,d0
	rts
