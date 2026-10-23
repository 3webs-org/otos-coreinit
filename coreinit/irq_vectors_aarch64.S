/* ARMv8 EL1 vector table. Each slot is 128 bytes; VBAR_EL1 requires a
 * 2 KiB-aligned table. IRQ and synchronous exceptions save every GPR and
 * ELR/SPSR, dispatch to separate C handlers, and ERET only when resolved.
 * FIQ and SError halt rather than retrying indefinitely. */
.text
.p2align 11
.global _irq_aarch64_vectors
_irq_aarch64_vectors:
.macro VECTOR target
    b \target
    .balign 128
.endm
/* Current EL using SP_EL0. */
VECTOR _irq_aarch64_sync_entry
VECTOR _irq_aarch64_entry
VECTOR _irq_aarch64_unhandled
VECTOR _irq_aarch64_unhandled
/* Current EL using SP_EL1. */
VECTOR _irq_aarch64_sync_entry
VECTOR _irq_aarch64_entry
VECTOR _irq_aarch64_unhandled
VECTOR _irq_aarch64_unhandled
/* Lower EL using AArch64. */
VECTOR _irq_aarch64_sync_entry
VECTOR _irq_aarch64_entry
VECTOR _irq_aarch64_unhandled
VECTOR _irq_aarch64_unhandled
/* Lower EL using AArch32. */
VECTOR _irq_aarch64_sync_entry
VECTOR _irq_aarch64_entry
VECTOR _irq_aarch64_unhandled
VECTOR _irq_aarch64_unhandled

_irq_aarch64_unhandled:
    msr daifset, #0xf
1:  wfe
    b 1b

/* 31 GPRs + ELR_EL1 + SPSR_EL1, rounded to 16-byte stack alignment.
 * Preserve every register including x18. Synchronous data aborts and
 * asynchronous IRQs use the same frame, then separate dispatchers. */
.macro SAVE_AND_DISPATCH handler
    sub sp, sp, #272
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x9, [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp x18, x19, [sp, #144]
    stp x20, x21, [sp, #160]
    stp x22, x23, [sp, #176]
    stp x24, x25, [sp, #192]
    stp x26, x27, [sp, #208]
    stp x28, x29, [sp, #224]
    str x30, [sp, #240]
    mrs x0, elr_el1
    str x0, [sp, #248]
    mrs x0, spsr_el1
    str x0, [sp, #256]
    bl \handler
    b _irq_aarch64_restore
.endm

_irq_aarch64_sync_entry:
    SAVE_AND_DISPATCH _pagefault_aarch64_handle

_irq_aarch64_entry:
    SAVE_AND_DISPATCH _irq_aarch64_dispatch

_irq_aarch64_restore:
    ldr x0, [sp, #248]
    msr elr_el1, x0
    ldr x0, [sp, #256]
    msr spsr_el1, x0
    ldr x30, [sp, #240]
    ldp x28, x29, [sp, #224]
    ldp x26, x27, [sp, #208]
    ldp x24, x25, [sp, #192]
    ldp x22, x23, [sp, #176]
    ldp x20, x21, [sp, #160]
    ldp x18, x19, [sp, #144]
    ldp x16, x17, [sp, #128]
    ldp x14, x15, [sp, #112]
    ldp x12, x13, [sp, #96]
    ldp x10, x11, [sp, #80]
    ldp x8, x9, [sp, #64]
    ldp x6, x7, [sp, #48]
    ldp x4, x5, [sp, #32]
    ldp x2, x3, [sp, #16]
    ldp x0, x1, [sp, #0]
    add sp, sp, #272
    eret
