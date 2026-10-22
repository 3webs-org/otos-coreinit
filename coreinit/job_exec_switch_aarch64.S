/* SPDX-License-Identifier: 0BSD */

/* void job_exec_switch(uint64_t *save_to, uint64_t load_from)
 *
 * Saves the CURRENTLY running job's full register state -- x0-x30, plus
 * the sp it will resume at, plus its own DAIF -- into a 264-byte frame,
 * writes the frame's base address to *save_to, then loads the SAME
 * layout from load_from (another job's own saved_rsp, however it got
 * there: a previous call to this function, or job_exec_init's fake
 * first frame) and branches into it.
 *
 * This is deliberately the SAME restore path a real hardware interrupt
 * will eventually use -- to this code, a job that has never actually
 * run yet and a job that's genuinely been switched away from several
 * times look identical: both are just "a set of register values to
 * load, plus a PC to branch to." (Unifying the two for real -- an
 * exception vector that builds this exact frame and a resume via ERET
 * instead of br -- is the exception-vector unit's work, not this
 * file's.)
 *
 * INTERRUPT SAFETY: from the moment SP starts being repointed at
 * another job's frame (midway through being consumed by the loads
 * below) until the resume completes, an interrupt landing here would
 * push its own frame onto whatever SP currently is -- neither a
 * complete "job about to be saved" stack nor a valid "job about to
 * run" stack, corrupting either one. The fix mirrors the x86_64 file's:
 * mask interrupts (DAIFSet, bit I) immediately, before any stack
 * pointer manipulation begins, and let the resuming job's OWN saved
 * DAIF be the only thing that ever un-masks them -- a blanket
 * un-mask would permanently silence a job that had deliberately run
 * with IRQs off. The outgoing job's saved DAIF must reflect its TRUE,
 * pre-mask state (interrupts enabled, if that's what it had) so that
 * when it's later resumed it doesn't come back with interrupts
 * permanently disabled -- captured with mrs daif BEFORE msr daifset,
 * not after, exactly the pushfq-before-cli discipline of
 * job_exec_switch_x86_64.S.
 *
 * Field layout matches job_saved_context_aarch64_t
 * (job_exec_aarch64.h) EXACTLY -- there is no type system connecting
 * this file's raw offsets to that struct's field order, only matching
 * them by hand on both sides:
 *     0  x0        64  x8       136  x17
 *     8  x1        72  x9       144  x18
 *    16  x2        80  x10      152  x19
 *    24  x3        88  x11      160  x20
 *    32  x4        96  x12      168  x21
 *    40  x5       104  x13      176  x22
 *    48  x6       112  x14      184  x23
 *    56  x7       120  x15      192  x24
 *                        200  x25
 *                        208  x26
 *                        216  x27
 *                        224  x28
 *                        232  x29
 *                        240  x30  (resume PC)
 *                        248  sp   (resume SP)
 *                        256  pstate (saved DAIF)
 *   Total frame size: 264 bytes (33 qwords).
 *
 * AAPCS on entry: x0 = save_to, x1 = load_from, and sp exactly as the
 * caller left it (16-byte aligned at the call site). Unlike x86, `bl`
 * pushes NOTHING -- the return address went into x30 -- so saving x30
 * into the frame's x30 slot is what means "resume right after this
 * call," the analogue of the x86 file reading the pushed return address
 * back off the stack. Every offset below is relative to the frame base
 * once the frame is carved out: parked x9 at frame_base + 264, entry sp
 * (= the resumed sp) at frame_base + 280.
 *
 * THE PARKED-x9 TRICK: x9 is one of the registers this frame saves, but
 * it is also the only scratch register that can hold the DAIF value
 * captured by `mrs x9, daif` -- so its ENTRY value would be lost. It is
 * parked first, with str x9, [sp, #-16]!, in a 16-byte-aligned slot
 * just below sp (the frame is then 264 bytes below that, which leaves
 * the frame BASE 8-byte rather than 16-byte aligned -- harmless, since
 * every field is a qword and the Q-form stp/ldr only need 8), and read
 * back by fixed offset
 * into the frame's x9 slot. An interrupt landing before the mask takes
 * effect pushes its own frame BELOW the parked slot, so the parked
 * value is untouched -- the same harmlessness the x86 file gets from
 * pushfq-before-cli. */

.text
.global _job_exec_switch
_job_exec_switch:
    /* No return address sits on the stack here -- `bl` left it in x30,
     * which the save below stores at offset 240, so a job resumed from
     * this frame comes back right after its own call. */

    /* Park x9's entry value (see the note above), then capture the
     * TRUE, pre-mask DAIF, then mask interrupts for the remainder of
     * this routine -- see the interrupt-safety note above. */
    str   x9, [sp, #-16]!
    mrs   x9, daif
    msr   daifset, #2        /* mask IRQs (bit I) */

    sub   sp, sp, #264       /* frame base (8-byte aligned: 280 % 16 = 8,
                              * fine for qword-only fields) */

    stp   x0, x1, [sp, #0]
    stp   x2, x3, [sp, #16]
    stp   x4, x5, [sp, #32]
    stp   x6, x7, [sp, #48]
    str   x8, [sp, #64]
    /* x9's slot is filled last, from the parked value -- x9 itself is
     * currently holding the DAIF capture, not the job's x9. */
    stp   x10, x11, [sp, #80]
    stp   x12, x13, [sp, #96]
    stp   x14, x15, [sp, #112]
    stp   x16, x17, [sp, #128]
    str   x18, [sp, #144]
    stp   x19, x20, [sp, #152]
    stp   x21, x22, [sp, #168]
    stp   x23, x24, [sp, #184]
    stp   x25, x26, [sp, #200]
    stp   x27, x28, [sp, #216]
    stp   x29, x30, [sp, #232]

    /* "Resume sp": what this job's own sp will be once this function
     * truly returns -- its ENTRY sp, unchanged, which is frame_base +
     * 280 (the 16-byte parked-x9 slot plus this 264-byte frame). The
     * x86 file's analogue is its entry_rsp + 8, and the +8 there is a
     * real stack slot: x86's `call` pushed a return address that `ret`
     * pops. AArch64 has no such slot -- `bl` left the return address in
     * x30, which the save above stored at offset 240 -- so nothing is
     * consumed on the way back and resume_sp is the entry sp itself,
     * still 16-byte aligned exactly as AAPCS requires it wherever the
     * resumed job can observe it. (The frame BASE is only 8-byte
     * aligned, since 280 is not a multiple of 16; harmless, because
     * every field is a qword and the Q-form stp/ldr need 8.) */
    add   x10, sp, #280
    str   x10, [sp, #248]    /* sp */

    /* Recover the parked entry value of x9 (frame_base + 264, the slot
     * str x9,[sp,#-16]! wrote before the frame was carved out) and
     * store it in x9's own slot. */
    ldr   x10, [sp, #264]
    str   x10, [sp, #72]     /* x9 */

    /* The DAIF captured BEFORE the mask -- this job's own true
     * interrupt state, not the post-mask one. */
    str   x9, [sp, #256]     /* pstate */

    /* Save complete -- hand the base of this frame to the caller. (sp
     * is not encodable as str's Rt, hence the copy through x10, which
     * is free here: the job's own x10 was already stored above.) */
    mov   x10, sp
    str   x10, [x0]

    /* Load the other job's frame and resume it. Still inside the
     * interrupt-masked window from the daifset above -- nothing here
     * can be interrupted until the msr daif below, which uses the
     * RESUMING job's own saved DAIF (see the note above). x1 still
     * holds load_from; x0 is no longer needed. */
    mov   x10, x1            /* frame base */

    /* Callee-saved first, then the caller-saved half -- the order is
     * only constrained by which registers are still holding live
     * switch machinery (x10, and x11/x12 as scratch below). */
    ldp   x19, x20, [x10, #152]
    ldp   x21, x22, [x10, #168]
    ldp   x23, x24, [x10, #184]
    ldp   x25, x26, [x10, #200]
    ldp   x27, x28, [x10, #216]
    ldr   x29, [x10, #232]   /* frame pointer (x30 loaded separately) */
    ldr   x30, [x10, #240]   /* THE RESUME PC -- the ARM rip */
    ldr   x11, [x10, #248]   /* resume sp, into a scratch register */
    ldr   x12, [x10, #256]   /* the resuming job's OWN DAIF */

    ldp   x0, x1, [x10, #0]
    ldp   x2, x3, [x10, #16]
    ldp   x4, x5, [x10, #32]
    ldp   x6, x7, [x10, #48]
    ldr   x8, [x10, #64]
    ldr   x9, [x10, #72]
    ldr   x13, [x10, #104]
    ldr   x14, [x10, #112]
    ldr   x15, [x10, #120]
    ldr   x16, [x10, #128]
    ldr   x17, [x10, #136]
    ldr   x18, [x10, #144]

    /* Un-mask using the RESUMING job's own saved DAIF -- the analogue
     * of iretq's atomic RFLAGS restore, with one honest difference:
     * `msr daif` needs a LIVE register as its source, so it cannot be
     * the last instruction the way iretq is -- by the time every field
     * is loaded, every register holds resumed-job data and nothing is
     * left to carry the DAIF value in. That leaves a window between
     * this instruction and the branch below. It is as small as this
     * design can make it (the sp move plus three self-loads), and an
     * interrupt taken inside it runs with the resuming job's own
     * interrupt state and its own stack already installed -- but it
     * would still clobber the caller-saved registers loaded just above,
     * so this window is NOT safe the way iretq's is. Closing it
     * properly means resuming via ERET off a real exception frame,
     * which is the exception-vector unit's work, not this file's; until
     * that exists, nothing may enable IRQs around job_exec_switch on
     * aarch64. */
    msr   daif, x12
    mov   sp, x11

    /* x10 and x11/x12 are the last things loaded, because they were the
     * scratch registers carrying the frame base and the sp/DAIF values
     * through the sequence above -- each self-loads from the frame only
     * once its scratch use is finished. */
    ldr   x11, [x10, #88]    /* resumed x11 */
    ldr   x12, [x10, #96]    /* resumed x12 */
    ldr   x10, [x10, #80]    /* resumed x10 -- self-load, so it goes last */
    br    x30
