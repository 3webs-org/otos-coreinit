#include "job_exec.h"

static uint8_t g_job_stacks[JOB_EXEC_MAX_RESIDENT][JOB_EXEC_STACK_SIZE]
    __attribute__((aligned(16)));
static int g_job_stack_used[JOB_EXEC_MAX_RESIDENT];

void job_exec_alloc_stack(uint64_t *out_top, int *out_slot)
{
    for (int i = 0; i < JOB_EXEC_MAX_RESIDENT; i++) {
        if (!g_job_stack_used[i]) {
            g_job_stack_used[i] = 1;
            *out_slot = i;
            *out_top =
                (uint64_t)(uintptr_t)(g_job_stacks[i] + JOB_EXEC_STACK_SIZE);
            return;
        }
    }
    *out_slot = -1;
    *out_top = 0;
}

void job_exec_free_stack(int slot)
{
    if (slot >= 0 && slot < JOB_EXEC_MAX_RESIDENT) {
        g_job_stack_used[slot] = 0;
    }
}

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

/* --- Per-core job table and the wait/wake scheduler --- */

static job_exec_slot_t g_job_table[JOB_EXEC_MAX_RESIDENT];
static int g_current_job = -1; /* index of whatever's running on THIS
                                * core, -1 if we're in a non-job
                                * context (bootstrap, or a driver that
                                * kicked off the first job) */

static job_exec_slot_t *pick_highest_priority_ready(void)
{
    job_exec_slot_t *best = NULL;
    for (int i = 0; i < JOB_EXEC_MAX_RESIDENT; i++) {
        if (g_job_table[i].state == JOB_EXEC_READY) {
            if (!best || g_job_table[i].priority > best->priority) {
                best = &g_job_table[i];
            }
        }
    }
    return best;
}

int job_exec_spawn(uint64_t saved_rsp, int priority)
{
    for (int i = 0; i < JOB_EXEC_MAX_RESIDENT; i++) {
        if (g_job_table[i].state == JOB_EXEC_UNUSED) {
            g_job_table[i].saved_rsp = saved_rsp;
            g_job_table[i].priority = priority;
            g_job_table[i].state = JOB_EXEC_READY;
            g_job_table[i].waiting_on = NULL;
            return i;
        }
    }
    return -1;
}

void job_exec_wait(job_wait_queue_t *q)
{
    int me = g_current_job;
    g_job_table[me].state = JOB_EXEC_BLOCKED;
    g_job_table[me].waiting_on = q;

    job_exec_slot_t *next = pick_highest_priority_ready();
    if (!next) {
        /* Nothing else ready at all -- Phase A has no real idle job
         * yet to fall back to; this is a genuine "nothing left to run"
         * condition, not something to paper over. */
        for (;;) {
            __asm__ volatile("hlt");
        }
    }
    int next_idx = (int)(next - g_job_table);
    g_job_table[next_idx].state = JOB_EXEC_RUNNING;
    g_current_job = next_idx;
    job_exec_switch(&g_job_table[me].saved_rsp, g_job_table[next_idx].saved_rsp);
    /* Resumes here, transparently, once job_exec_wake(q) picks this
     * slot again and it's actually rescheduled. */
}

void job_exec_wake(job_wait_queue_t *q)
{
    job_exec_slot_t *woken = NULL;
    for (int i = 0; i < JOB_EXEC_MAX_RESIDENT; i++) {
        if (g_job_table[i].state == JOB_EXEC_BLOCKED &&
            g_job_table[i].waiting_on == q) {
            if (!woken || g_job_table[i].priority > woken->priority) {
                woken = &g_job_table[i];
            }
        }
    }
    if (!woken) {
        return; /* nobody waiting on q */
    }

    int woken_idx = (int)(woken - g_job_table);
    g_job_table[woken_idx].state = JOB_EXEC_READY;
    g_job_table[woken_idx].waiting_on = NULL;

    int cur = g_current_job;
    if (cur >= 0 && g_job_table[woken_idx].priority > g_job_table[cur].priority) {
        /* The woken job outranks whoever's calling wake() -- preempt
         * immediately, right here. The caller is READY (not BLOCKED --
         * it isn't waiting on anything), so it'll run again as soon as
         * it's next the highest-priority ready job. */
        g_job_table[cur].state = JOB_EXEC_READY;
        g_job_table[woken_idx].state = JOB_EXEC_RUNNING;
        g_current_job = woken_idx;
        job_exec_switch(&g_job_table[cur].saved_rsp,
                        g_job_table[woken_idx].saved_rsp);
        /* Resumes here once this job is rescheduled again. */
    }
    /* Otherwise: woken job just sits READY; caller continues normally,
     * no switch happens now. */
}

uint64_t *job_exec_current_rsp_slot(void)
{
    return &g_job_table[g_current_job].saved_rsp;
}

void job_exec_run(uint64_t *save_rsp_to, int slot)
{
    g_job_table[slot].state = JOB_EXEC_RUNNING;
    g_current_job = slot;
    job_exec_switch(save_rsp_to, g_job_table[slot].saved_rsp);
}
