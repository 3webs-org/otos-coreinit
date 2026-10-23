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
 * x0 receives the address of a struct grub_relocator64_state. Every
 * field is loaded into its register, sp is set, and we branch to pc.
 * There is no return path, no stack frame of our own, and no use of the
 * C stack after sp is changed -- that ordering matters, because sp now
 * points into the kernel's heap and anything below would be writing
 * over memory the kernel is about to receive.
 *
 * Order of the loads is deliberate: the state pointer lives in x0, so
 * x0 is loaded LAST, from x1, which is why x1 is staged first. Loading
 * x0 before reading the remaining fields would lose the pointer. */

	.text

	.globl grub_relocator64_boot_asm
	.type grub_relocator64_boot_asm, %function
grub_relocator64_boot_asm:
	mov	x1, x0			/* x1 = state pointer */

	ldr	x2, [x1, #8]		/* state->x1 */
	ldr	x3, [x1, #16]		/* state->x2 */
	ldr	x4, [x1, #24]		/* state->x3 */
	ldr	x5, [x1, #40]		/* state->pc   */
	ldr	x6, [x1, #32]		/* state->sp   */
	ldr	x7, [x1, #0]		/* state->x0   */

	mov	x0, x7
	mov	sp, x6
	br	x5

	.size grub_relocator64_boot_asm, . - grub_relocator64_boot_asm
