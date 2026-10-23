/* The trusted securinit wrapper performs the EL1→EL0 drop itself. The
 * untrusted probe is copied into a separate EL0-only mapping, not called
 * from the privileged securinit image. saski supplies no invoke trap. */
.text
.globl _securinit_arm_run_user
_securinit_arm_run_user:
    stp x29, x30, [sp, #-16]!
    stp x19, x20, [sp, #-16]!
    stp x21, x22, [sp, #-16]!
    mov x19, x0             /* EL0 entry */
    mov x20, x1             /* EL0 stack */
    mov x21, x2             /* deliberately forbidden Admin address */
    adr x22, 1f
    str x22, [x3]           /* trusted EL1 resume address */
    mrs x22, daif
    str x22, [x4]           /* restore original interrupt mask on exit */
    msr sp_el0, x20
    mov x0, xzr
    mov x1, x21
    msr elr_el1, x19
    msr spsr_el1, xzr      /* EL0t, interrupts enabled */
    isb
    eret
1:
    ldp x21, x22, [sp], #16
    ldp x19, x20, [sp], #16
    ldp x29, x30, [sp], #16
    ret

/* The first instruction MUST Data Abort with ESR EC=0x24: EL0 has no
 * read permission for the Admin record in securinit's EL1-only image.
 * The wrapper callback advances ELR by one instruction, then BRK exits
 * through the same lower-EL synchronous vector. */
.globl _securinit_arm_probe_start
_securinit_arm_probe_start:
    ldr x2, [x1]
    brk #0
.globl _securinit_arm_probe_end
_securinit_arm_probe_end:
