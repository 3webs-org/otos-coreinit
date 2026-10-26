/* SPDX-License-Identifier: 0BSD */
/* otos_bump_alloc/otos_bump_finalize: the real implementation behind
 * include/boot_protocol.h's struct boot_allocator. Hand-written
 * assembly, not C, for the same reason this project's other
 * copied-and-executed-from-an-arbitrary-address code already is
 * (saski's own entry points, the GRUB relocator's boot_asm routines):
 * a C compiler's position-independence guarantees are about whatever
 * ONE binary it is building as a whole, not about lifting one
 * function's own machine code out and running it, unmodified, from a
 * completely different address inside a DIFFERENT binary than the one
 * that compiled it -- which is exactly what every producer of this
 * object does (see otos-boot/tools/build-stub.py,
 * otos-grub/loader/arm64/otos_grub.c). This routine touches nothing
 * but its own two arguments and immediates, so it is trivially
 * position-independent by construction -- no ADR, no literal pool,
 * nothing to get wrong by moving it.
 *
 * Producers: assemble this file once, copy its machine code bytes
 * (otos_bump_alloc_start .. otos_bump_alloc_end) into the SAME
 * reserved region a struct boot_allocator's own region_base/region_len
 * describe (or anywhere else that outlives the bootloader), and point
 * that struct's alloc/finalize fields at wherever the copy landed.
 *
 * struct boot_allocator's own layout, restated here since this file
 * only has offsets into it, not the real C struct:
 *   +0  magic
 *   +8  alloc (function pointer -- this struct's own address, +0 from
 *       otos_bump_alloc_start)
 *   +16 finalize (function pointer -- otos_bump_finalize's own offset
 *       from otos_bump_alloc_start)
 *   +24 region_base
 *   +32 region_len
 *   +40 used
 *   +48 finalized
 * Keep in sync by hand with include/boot_protocol.h's own struct --
 * same convention every other wire-shape constant in this project
 * already uses. */

    .text

/* void *otos_bump_alloc(struct boot_allocator *self, uint64_t size,
 *                       uint64_t align);
 * x0 = self, x1 = size, x2 = align (0 means 16). Returns the allocated
 * region's own physical address in x0, or 0 if it would not fit, or if
 * finalize has already run. */
    .global otos_bump_alloc_start
    .global otos_bump_alloc
    .type otos_bump_alloc, %function
otos_bump_alloc_start:
otos_bump_alloc:
    ldr x9, [x0, #48]              /* self->finalized */
    cbnz x9, .Lbump_alloc_fail
    cbnz x2, 1f
    mov x2, #16
1:
    sub x3, x2, #1                 /* align - 1 */
    ldr x4, [x0, #40]              /* self->used */
    add x5, x4, x3
    bic x5, x5, x3                 /* aligned_used = (used+align-1) & ~(align-1) */
    add x6, x5, x1                 /* new_used = aligned_used + size */
    ldr x7, [x0, #32]              /* self->region_len */
    cmp x6, x7
    b.hi .Lbump_alloc_fail
    str x6, [x0, #40]              /* self->used = new_used */
    ldr x8, [x0, #24]              /* self->region_base */
    add x0, x8, x5                 /* result = region_base + aligned_used */
    ret
.Lbump_alloc_fail:
    mov x0, #0
    ret
    .size otos_bump_alloc, . - otos_bump_alloc

/* void otos_bump_finalize(struct boot_allocator *self);
 * x0 = self. Shrinks region_len to used and sets finalized=1 -- see
 * include/boot_protocol.h's own comment on struct boot_allocator's
 * finalize field for why this is unconditional and explicit, not just
 * a side effect of region_len==used. */
    .global otos_bump_finalize
    .type otos_bump_finalize, %function
otos_bump_finalize:
    ldr x1, [x0, #40]              /* self->used */
    str x1, [x0, #32]              /* self->region_len = used */
    mov x1, #1
    str x1, [x0, #48]              /* self->finalized = 1 */
    ret
    .size otos_bump_finalize, . - otos_bump_finalize

    .global otos_bump_alloc_end
otos_bump_alloc_end:
