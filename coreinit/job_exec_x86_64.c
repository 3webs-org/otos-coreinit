#include "job_exec.h"
#include "job_exec_x86_64.h"

uint64_t job_exec_init(void *stack_top, void (*entry)(uint64_t arg),
                       uint64_t arg)
{
    /* Stack grows down; carve the frame out just below stack_top,
     * 16-byte aligned -- matches what a real interrupt would have
     * found (the SysV ABI, and this project's own IDT gates, both
     * assume a conventionally-aligned stack). */
    uintptr_t top = (uintptr_t)stack_top;
    top &= ~(uintptr_t)0xF;
    job_saved_context_t *frame =
        (job_saved_context_t *)(top - sizeof(job_saved_context_t));

    frame->r15 = 0;
    frame->r14 = 0;
    frame->r13 = 0;
    frame->r12 = 0;
    frame->r11 = 0;
    frame->r10 = 0;
    frame->r9 = 0;
    frame->r8 = 0;
    frame->rdi = arg; /* SysV ABI: first integer argument */
    frame->rsi = 0;
    frame->rbp = 0;
    frame->rbx = 0;
    frame->rdx = 0;
    frame->rcx = 0;
    frame->rax = 0;

    frame->rip = (uint64_t)(uintptr_t)entry;
    frame->cs = JOB_EXEC_CODE_SELECTOR;
    frame->rflags = JOB_EXEC_INITIAL_RFLAGS;
    frame->rsp = top; /* this job's own rsp once running -- back at the
                       * (aligned) top of its own stack, as if it had
                       * just been interrupted right after a function
                       * prologue that hadn't pushed anything yet */
    frame->ss = JOB_EXEC_DATA_SELECTOR;

    return (uint64_t)(uintptr_t)frame;
}
