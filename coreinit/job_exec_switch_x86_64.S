/* SPDX-License-Identifier: 0BSD */

/* void job_exec_switch(uint64_t *save_rsp_to, uint64_t load_rsp_from)
 *
 * Saves the CURRENTLY running job's full register state -- all GPRs
 * plus an iretq-compatible frame (rip/cs/rflags/rsp/ss) representing
 * "resume right after this call returns" -- onto its OWN stack, writes
 * the resulting base address to *save_rsp_to, then loads the SAME
 * layout from load_rsp_from (another job's own saved_rsp, however it
 * got there: a previous call to this function, or Phase 3's timer ISR
 * doing the equivalent save) and iretq's into it.
 *
 * This is deliberately the SAME restore path a real hardware interrupt
 * will use in Phase 3 -- iretq doesn't know or care whether the frame
 * it's consuming came from real hardware or was hand-built here, which
 * is exactly what makes job_exec_init's fake first frame (job_exec.c)
 * work: to this function, a job that's never actually run yet and a
 * job that's genuinely been switched away from several times look
 * identical.
 *
 * INTERRUPT SAFETY: from the moment RSP starts being repointed at
 * another job's frame (partway through being consumed by the pop
 * sequence below) until iretq completes, a hardware interrupt landing
 * here would push its own frame onto whatever RSP currently is --
 * neither a complete "job about to be saved" stack nor a valid "job
 * about to run" stack, corrupting either one. This was a real,
 * previously unguarded bug (traced to a rare, ~1-in-10 triple fault
 * whose actual cause -- a corrupted, wrapped-to-near-zero RSP -- only
 * became visible once #DF/#GP got their own IST-based diagnostic
 * stack; see irq.c's setup_tss_and_ist). The fix mirrors what a real
 * interrupt gate already does: cli immediately, before any stack
 * pointer manipulation begins, and let iretq's own atomic RFLAGS
 * restore be the only thing that ever re-enables interrupts -- for
 * whichever job iretq is resuming into, using THAT job's own saved
 * IF state, not a blanket re-enable. The outgoing job's own saved
 * RFLAGS must reflect its TRUE, pre-cli state (interrupts enabled, if
 * that's what it had) so that when it's later resumed, it doesn't come
 * back with interrupts permanently disabled -- captured via pushfq
 * BEFORE cli, not after.
 *
 * Field layout matches job_saved_context_t (job_exec.h) EXACTLY --
 * there is no type system connecting this file's raw offsets to that
 * struct's field order, only matching them by hand on both sides:
 *   0   r15      64  rdi      128 cs
 *   8   r14      72  rsi      136 rflags
 *   16  r13      80  rbp      144 rsp
 *   24  r12      88  rbx      152 ss
 *   32  r11      96  rdx
 *   40  r10      104 rcx
 *   48  r9       112 rax
 *   56  r8       120 rip
 * Total frame size: 160 bytes.
 *
 * SysV ABI on entry: rdi = save_rsp_to, rsi = load_rsp_from. */

.text
.global _job_exec_switch
_job_exec_switch:
    /* [rsp+0] = return address, pushed by the caller's own `call`. */

    /* Capture the TRUE, pre-cli RFLAGS first (while rsp is still the
     * caller's own, untouched stack), then disable interrupts for the
     * remainder of this routine -- see the interrupt-safety note
     * above. pushfq/cli here nets to zero extra stack usage by the
     * time the frame below is built: the pushed flags and the return
     * address both stay exactly where they land, read back by fixed
     * offset later rather than popped. */
    pushfq
    cli

    sub $160, %rsp

    movq %r15, 0(%rsp)
    movq %r14, 8(%rsp)
    movq %r13, 16(%rsp)
    movq %r12, 24(%rsp)
    movq %r11, 32(%rsp)
    movq %r10, 40(%rsp)
    movq %r9,  48(%rsp)
    movq %r8,  56(%rsp)
    movq %rdi, 64(%rsp)
    movq %rsi, 72(%rsp)
    movq %rbp, 80(%rsp)
    movq %rbx, 88(%rsp)
    movq %rdx, 96(%rsp)
    movq %rcx, 104(%rsp)
    movq %rax, 112(%rsp)

    /* The original return address is sitting untouched at
     * [new_rsp + 168] (= [entry_rsp + 0]): 160 bytes for this frame,
     * plus the 8 bytes pushfq consumed below it. */
    movq 168(%rsp), %rax
    movq %rax, 120(%rsp)     /* rip */

    movq $0x08, 128(%rsp)    /* cs -- JOB_EXEC_CODE_SELECTOR */

    /* The flags pushfq captured BEFORE cli -- this job's own true
     * interrupt-enable state, not the post-cli one. */
    movq 160(%rsp), %rax
    movq %rax, 136(%rsp)     /* rflags */

    /* "Resume rsp": what the caller's own rsp will be once this
     * function truly returns -- entry_rsp + 8 (one qword, for the
     * return address a real `ret` would pop) = new_rsp + 160 + 8 (this
     * frame) + 8 (the pushfq slot) = new_rsp + 176. */
    lea 176(%rsp), %rax
    movq %rax, 144(%rsp)     /* rsp */

    movq $0x10, 152(%rsp)    /* ss -- JOB_EXEC_DATA_SELECTOR */

    /* Save complete -- hand the base of this frame to the caller. */
    movq %rsp, (%rdi)

    /* Load the other job's frame and resume it. Still inside the
     * interrupt-disabled window from the cli above -- nothing here can
     * be interrupted until iretq's own atomic RFLAGS restore, using
     * the RESUMING job's own saved IF state (see the note above). */
    movq %rsi, %rsp

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
    /* rsp now points at [rip, cs, rflags, rsp, ss] -- exactly what
     * iretq expects, whether this frame came from a real interrupt or
     * was hand-built (job_exec_init, or the save sequence above). */
    iretq
