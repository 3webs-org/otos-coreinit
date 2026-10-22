#include "job_exec.h"
#include "job_exec_aarch64.h"

/* The ARM fake first frame -- same principle as the x86_64 version in
 * job_exec_x86_64.c: to the restore path in job_exec_switch_aarch64.S,
 * a job that has never run and a job that was switched away from look
 * identical, because both are just "a set of register values to load,
 * plus a PC to branch to." Nothing here knows which case it built. */
uint64_t job_exec_init(void *stack_top, void (*entry)(uint64_t arg),
                       uint64_t arg)
{
    /* Stack grows down; align the top to 16 (AAPCS requires a
     * 16-byte-aligned sp at every point a function may observe it) and
     * carve the frame out just below it. */
    uintptr_t top = (uintptr_t)stack_top;
    top &= ~(uintptr_t)0xF;
    job_saved_context_aarch64_t *frame = (job_saved_context_aarch64_t *)(
        top - sizeof(job_saved_context_aarch64_t));

    frame->x0 = arg; /* AAPCS: first integer argument in x0 -- the
                      * analogue of the x86 frame's rdi */
    frame->x1 = 0;
    frame->x2 = 0;
    frame->x3 = 0;
    frame->x4 = 0;
    frame->x5 = 0;
    frame->x6 = 0;
    frame->x7 = 0;
    frame->x8 = 0;
    frame->x9 = 0;
    frame->x10 = 0;
    frame->x11 = 0;
    frame->x12 = 0;
    frame->x13 = 0;
    frame->x14 = 0;
    frame->x15 = 0;
    frame->x16 = 0;
    frame->x17 = 0;
    frame->x18 = 0;
    frame->x19 = 0;
    frame->x20 = 0;
    frame->x21 = 0;
    frame->x22 = 0;
    frame->x23 = 0;
    frame->x24 = 0;
    frame->x25 = 0;
    frame->x26 = 0;
    frame->x27 = 0;
    frame->x28 = 0;
    frame->x29 = 0; /* no caller's frame pointer -- this job has no
                     * caller it will ever return to */
    frame->x30 = (uint64_t)(uintptr_t)entry; /* the resume PC */
    frame->sp = top; /* this job's own sp once running -- back at the
                      * (aligned) top of its own stack, as if it had
                      * just been interrupted before pushing anything */
    frame->pstate = JOB_EXEC_INITIAL_PSTATE;

    return (uint64_t)(uintptr_t)frame;
}
