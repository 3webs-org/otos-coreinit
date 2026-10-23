/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2009  Free Software Foundation, Inc.
 *
 *  GRUB is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  GRUB is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with GRUB.  If not, see <http://www.gnu.org/licenses/>.
 */

/* The aarch64 equivalent of grub-core/lib/i386/relocator64.S: the last
 * few instructions before control leaves GRUB for good.
 *
 * x0 receives the address of a struct grub_relocator64_state. Every field
 * is staged in a scratch register first -- the state pointer lives in x0,
 * so the argument registers are written only at the very end.
 *
 * The kernel is entered the way an arm64 XNU kernel expects: interrupts
 * masked, MMU and data cache off, x0 = boot-parameter block. Turning the
 * MMU off is safe here because UEFI's mapping is the identity map, so the
 * next instruction fetch lands at the same address untranslated. With the
 * data cache off every read goes to memory, so before that the loaded
 * image ([clean_start, clean_end)) and this routine's own code are cleaned
 * to the point of coherency. */

	.text

	.globl grub_relocator64_boot_asm
	.type grub_relocator64_boot_asm, %function
grub_relocator64_boot_asm:
0:	ldr	x2, [x0, #8]		/* state->x1 */
	ldr	x3, [x0, #16]		/* state->x2 */
	ldr	x4, [x0, #24]		/* state->x3 */
	ldr	x6, [x0, #32]		/* state->sp */
	ldr	x5, [x0, #40]		/* state->pc */
	ldr	x9, [x0, #48]		/* state->clean_start */
	ldr	x10, [x0, #56]		/* state->clean_end */
	ldr	x7, [x0, #0]		/* state->x0 */

	msr	daifset, #0xf

	mrs	x11, ctr_el0		/* DminLine: log2(words) of the line */
	ubfx	x11, x11, #16, #4
	mov	x12, #4
	lsl	x12, x12, x11		/* x12 = line size in bytes */
	sub	x13, x12, #1

	bic	x9, x9, x13
1:	cmp	x9, x10
	b.hs	2f
	dc	civac, x9
	add	x9, x9, x12
	b	1b

2:	adr	x9, 0b			/* local label: no relocation */
	bic	x9, x9, x13
	adr	x10, 9f
3:	cmp	x9, x10
	b.hs	4f
	dc	civac, x9
	add	x9, x9, x12
	b	3b

4:	dsb	sy
	mrs	x11, CurrentEL
	cmp	x11, #(2 << 2)
	b.eq	5f
	mrs	x11, sctlr_el1
	bic	x11, x11, #1		/* M */
	bic	x11, x11, #(1 << 2)	/* C */
	msr	sctlr_el1, x11
	b	6f
5:	mrs	x11, sctlr_el2
	bic	x11, x11, #1
	bic	x11, x11, #(1 << 2)
	msr	sctlr_el2, x11
6:	isb

	/* Firmware's own structures -- the system table, its configuration
	 * table, the DTB it installed there -- were written with the cache
	 * on and may still be dirty in it. The kernel reaches them through
	 * efi_system_table with caching off, so every data cache level up to
	 * the point of coherency is cleaned and invalidated by set/way, after
	 * C is cleared so no new lines are allocated meanwhile. */
	mrs	x11, clidr_el1
	ubfx	x12, x11, #24, #3	/* LoC */
	lsl	x12, x12, #1		/* LoC * 2, compared against level<<1 */
	mov	x13, #0			/* x13 = level << 1 */
10:	cmp	x13, x12
	b.hs	19f
	add	x14, x13, x13, lsr #1	/* level * 3 */
	lsr	x15, x11, x14
	and	x15, x15, #7		/* cache type at this level */
	cmp	x15, #2
	b.lo	18f			/* none or I-only */
	msr	csselr_el1, x13
	isb
	mrs	x15, ccsidr_el1
	and	x16, x15, #7
	add	x16, x16, #4		/* log2(line bytes) */
	ubfx	x17, x15, #3, #10	/* max way */
	clz	w8, w17			/* way shift */
	ubfx	x15, x15, #13, #15	/* max set */
11:	mov	x9, x15			/* set */
12:	lsl	x10, x17, x8
	orr	x10, x10, x13
	lsl	x14, x9, x16
	orr	x10, x10, x14
	dc	cisw, x10
	subs	x9, x9, #1
	b.ge	12b
	subs	x17, x17, #1
	b.ge	11b
18:	add	x13, x13, #2
	b	10b
19:	msr	csselr_el1, xzr
	dsb	sy
	isb
	ic	iallu
	dsb	sy
	isb

	mov	x1, x2
	mov	x2, x3
	mov	x3, x4
	mov	x0, x7
	mov	sp, x6
	br	x5
9:
	.size grub_relocator64_boot_asm, . - grub_relocator64_boot_asm
