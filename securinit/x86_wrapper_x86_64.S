/* Wrapper-owned CPL3 transition and exception gates. coreinit installs
 * these trusted entry addresses into its EXISTING IDT/TSS; saski owns no
 * invoke trap. INT 0x80 carries the one outgoing IPC request. INT3 ends
 * the one-shot guest and returns to the saved wrapper stack in ring 0. */
.text
.macro SAVE_REGS
    pushq %rax
    pushq %rcx
    pushq %rdx
    pushq %rbx
    pushq %rbp
    pushq %rsi
    pushq %rdi
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
.endm
.macro RESTORE_REGS
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rdi
    popq %rsi
    popq %rbp
    popq %rbx
    popq %rdx
    popq %rcx
    popq %rax
.endm

.globl _securinit_x86_int80
_securinit_x86_int80:
    SAVE_REGS
    movq %rsp, %rdi
    movl $0x80, %esi
    callq _securinit_x86_dispatch
    RESTORE_REGS
    iretq

.globl _securinit_x86_int3
_securinit_x86_int3:
    SAVE_REGS
    movq %rsp, %rdi
    movl $3, %esi
    callq _securinit_x86_dispatch
    testl %eax, %eax
    jnz 1f
    RESTORE_REGS
    iretq
1:
    callq _securinit_x86_resume_rsp
    movq %rax, %r12
    callq _securinit_x86_resume_pc
    movq %rax, %r13
    callq _securinit_x86_resume_flags
    movq %r12, %rsp
    pushq %rax
    popfq
    jmpq *%r13

.globl _securinit_x86_run_user
_securinit_x86_run_user:
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    subq $8, %rsp
    movq %rdi, %r12       /* guest RIP */
    movq %rsi, %r13       /* guest RSP */
    movq %rdx, %r14       /* Admin address, read must #PF */
    movq %rcx, %r15       /* guest result words */
    movq %r8, %rbx        /* bad endpoint descriptor */
    movq %r9, %rbp        /* granted Gate descriptor */
    movq %rsp, %rdi
    leaq 2f(%rip), %rsi
    callq _securinit_x86_save_resume
    movw $0x2b, %ax      /* user DS/ES, not kernel DPL0 selectors */
    movw %ax, %ds
    movw %ax, %es
    pushq $0x2b          /* user SS */
    pushq %r13           /* user stack */
    pushfq
    popq %rax
    orq $0x200, %rax     /* timer IRQs continue while guest runs */
    pushq %rax
    pushq $0x33          /* user code */
    pushq %r12           /* user RIP */
    movq %r14, %rsi
    movq %r15, %rdx
    movq %rbx, %r8
    movq %rbp, %r9
    xorl %eax, %eax
    iretq
2:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    xorl %eax, %eax
    addq $8, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbp
    popq %rbx
    retq

/* Guest code is copied into a 2 MiB region with only its PTEs USER=1.
 * Access to the privileged Admin record must fault at the first load.
 * After the wrapper advances RIP past it, INT80 tests the local Gate
 * endpoint and INT3 returns to the trusted resume label above. */
.globl _securinit_x86_probe_start
_securinit_x86_probe_start:
    movq (%rsi), %rbx
    movq %r8, %rdi
    int $0x80
    movq %rax, (%rdx)
    movq %r9, %rdi
    int $0x80
    movq %rax, 8(%rdx)
    int3
.globl _securinit_x86_probe_end
_securinit_x86_probe_end:
