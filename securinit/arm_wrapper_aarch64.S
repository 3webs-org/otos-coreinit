/* The trusted securinit wrapper performs the EL1→EL0 drop itself. The
 * untrusted probe is copied into a separate EL0-only mapping, not called
 * from the privileged securinit image. saski supplies no invoke trap. */
.text
.globl _securinit_arm_run_user
_securinit_arm_run_user:
    stp x29, x30, [sp, #-16]!
    stp x19, x20, [sp, #-16]!
    stp x21, x22, [sp, #-16]!
    stp x23, x24, [sp, #-16]!
    stp x25, x26, [sp, #-16]!
    mov x19, x0             /* EL0 entry */
    mov x20, x1             /* EL0 stack */
    mov x21, x2             /* deliberately forbidden Admin address */
    mov x22, x3             /* trusted EL1 resume address output */
    mov x23, x4             /* saved DAIF output */
    mov x24, x5             /* EL0 result words */
    mov x25, x6             /* ungranted endpoint descriptor */
    mov x26, x7             /* granted Gate descriptor */
    adr x9, 1f
    str x9, [x22]
    mrs x9, daif
    str x9, [x23]
    msr sp_el0, x20
    mov x0, xzr
    mov x1, x21
    mov x3, x24
    mov x4, x25
    mov x5, x26
    msr elr_el1, x19
    msr spsr_el1, xzr      /* EL0t, interrupts enabled */
    isb
    eret
1:
    ldp x25, x26, [sp], #16
    ldp x23, x24, [sp], #16
    ldp x21, x22, [sp], #16
    ldp x19, x20, [sp], #16
    ldp x29, x30, [sp], #16
    ret

/* EL0 cannot read Admin. The same svc #0 entry rejects both an ungranted
 * endpoint and a request pointer aimed at Admin before forwarding the
 * granted Gate call. BRK returns control to trusted EL1 code. */
.globl _securinit_arm_probe_start
_securinit_arm_probe_start:
    ldr x2, [x1]
    mov x0, x4
    svc #0
    str x0, [x3]
    add x0, x4, #0x40
    svc #0
    str x0, [x3, #8]
    mov x0, x5
    svc #0
    str x0, [x3, #16]
    brk #0
.globl _securinit_arm_probe_end
_securinit_arm_probe_end:
