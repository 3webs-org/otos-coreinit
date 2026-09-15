/* SPDX-License-Identifier: 0BSD */

/* This file is assembled and linked SEPARATELY from everything else in
 * this project, as a flat binary fixed at TRAMPOLINE_ADDR (see the build
 * script) -- not as an ELF or Mach-O object. An AP starts executing here,
 * in real mode, immediately after receiving SIPI; there is no loader of
 * any kind at that point, so this code's own absolute addresses are
 * fixed at build time to match exactly where coreinit will have already
 * copied these bytes before sending SIPI. This is a deliberate,
 * documented exception to "avoid baked addresses" elsewhere in this
 * project: the trampoline's location is a fixed design constant we
 * chose, not something discovered at runtime, so there is nothing to be
 * position-independent WITH RESPECT TO.
 *
 * Per-core data this trampoline needs (target GDT, target 64-bit entry
 * point, target stack, PML4 physical address) is written by coreinit
 * into a small fixed data area immediately following this code, BEFORE
 * SIPI is sent -- see ap_params_offset below and coreinit/smp.c.
 */

.code16
.global ap_trampoline_start
ap_trampoline_start:
    cli
    cld

    xor %ax, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss

    lgdt (ap_gdt32_ptr - ap_trampoline_start + 0x8000)

    mov %cr0, %eax
    or $0x1, %eax
    mov %eax, %cr0

    ljmp $0x08, $(pm32_entry - ap_trampoline_start + 0x8000)

.align 8
/* --- 32-bit protected mode GDT: null, 32-bit code, 32-bit data --- */
ap_gdt32:
    .quad 0
    .quad 0x00CF9A000000FFFF /* 32-bit code, base 0, limit 4G */
    .quad 0x00CF92000000FFFF /* 32-bit data, base 0, limit 4G */
ap_gdt32_ptr:
    .word . - ap_gdt32 - 1
    .long (ap_gdt32 - ap_trampoline_start) + 0x8000

/* --- Per-core parameters, written by coreinit before SIPI is sent.
 * Fixed offsets from the start of this blob so coreinit can locate them
 * without needing symbol information (it only has the raw bytes). --- */
.align 8
.global ap_params_offset
ap_params_offset:
ap_param_pml4:      .quad 0   /* physical address of this core's PML4 */
ap_param_gdt64_ptr: .quad 0   /* physical address of a 10-byte GDT pointer for the real (post-transition) 64-bit GDT */
ap_param_entry64:   .quad 0   /* 64-bit entry point to jump to */
ap_param_stack:     .quad 0   /* initial RSP for the 64-bit entry */
ap_param_region_base: .quad 0 /* passed in RDI to the 64-bit entry, per
                               * SysV calling convention -- this core's
                               * assigned region base, kernel_boot_ap's
                               * one and only argument */

.code32
pm32_entry:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss

    /* Load this core's own PML4 (already built by coreinit for this
     * specific core's own identity-mapped region before SIPI was sent). */
    mov $0x8000, %ebx
    add $(ap_param_pml4 - ap_trampoline_start), %ebx
    mov (%ebx), %eax
    mov %eax, %cr3

    mov %cr4, %eax
    or $0x20, %eax /* PAE */
    mov %eax, %cr4

    mov $0xC0000080, %ecx
    rdmsr
    or $0x100, %eax /* EFER.LME */
    wrmsr

    mov %cr0, %eax
    or $0x80000000, %eax /* PG */
    mov %eax, %cr0

    /* Load the REAL 64-bit GDT (the same layout/selectors the rest of
     * this project already assumes -- 0x08 = 64-bit code, 0x10 = 64-bit
     * data) rather than continuing to use this trampoline's own scratch
     * 32-bit-only GDT, so downstream code (irq.c's hardcoded selector,
     * etc.) needs no per-core-specific knowledge. */
    mov $0x8000, %ebx
    add $(ap_param_gdt64_ptr - ap_trampoline_start), %ebx
    mov (%ebx), %eax
    lgdt (%eax)

    ljmp $0x08, $(lm64_entry - ap_trampoline_start + 0x8000)

.code64
lm64_entry:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov %ax, %fs
    mov %ax, %gs

    mov $0x8000, %rbx
    add $(ap_param_stack - ap_trampoline_start), %rbx
    mov (%rbx), %rsp

    mov $0x8000, %rbx
    add $(ap_param_entry64 - ap_trampoline_start), %rbx
    mov (%rbx), %rax

    mov $0x8000, %rbx
    add $(ap_param_region_base - ap_trampoline_start), %rbx
    mov (%rbx), %rdi

    jmp *%rax

.global ap_trampoline_end
ap_trampoline_end:
