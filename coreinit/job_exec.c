#include "job_exec.h"
#include "arch_cpu.h"
#include "core_state.h"
#include "serial.h"

/* job_table/job_stack_used/current_job/job_stacks all now live in struct
 * core_state (see that header's own note on why the stacks are embedded
 * directly rather than a separate allocation). Every function below
 * fetches this core's own pointer once and operates through it -- see
 * each function's own comment for what happens if it's NULL, which
 * differs by function: some degrade the same way "pool exhausted" or
 * "table full" already did, but job_exec_wait/wake/run/
 * current_rsp_slot actually perform a context switch, where a NULL
 * core_state is not a condition any correctly-functioning caller can
 * reach (job_exec_spawn already refuses to hand out a job in that case),
 * so those panic instead of risking an unpredictable crash deep inside a
 * switch. */

void job_exec_alloc_stack(uint64_t *out_top, int *out_slot)
{
    struct core_state *cs = core_state_get();
    if (cs == NULL) {
        *out_slot = -1;
        *out_top = 0;
        return;
    }
    for (int i = 0; i < JOB_EXEC_MAX_RESIDENT; i++) {
        if (!cs->job_stack_used[i]) {
            cs->job_stack_used[i] = 1;
            *out_slot = i;
            *out_top =
                (uint64_t)(uintptr_t)(cs->job_stacks[i] + JOB_EXEC_STACK_SIZE);
            return;
        }
    }
    *out_slot = -1;
    *out_top = 0;
}

void job_exec_free_stack(int slot)
{
    struct core_state *cs = core_state_get();
    if (cs == NULL) {
        return;
    }
    if (slot >= 0 && slot < JOB_EXEC_MAX_RESIDENT) {
        cs->job_stack_used[slot] = 0;
    }
}

/* --- Per-core job table and the wait/wake scheduler --- */

/* The initial-frame builder is architecture-specific -- see
 * job_exec_x86_64.c / job_exec_aarch64.c, selected per-arch by
 * CMakeLists.txt. Everything below this point is arch-neutral
 * bookkeeping that only ever handles the opaque saved_rsp value that
 * builder (or the switch primitive) produced. */

static job_exec_slot_t *pick_highest_priority_ready(struct core_state *cs)
{
    job_exec_slot_t *best = NULL;
    for (int i = 0; i < JOB_EXEC_MAX_RESIDENT; i++) {
        if (cs->job_table[i].state == JOB_EXEC_READY) {
            if (!best || cs->job_table[i].priority > best->priority) {
                best = &cs->job_table[i];
            }
        }
    }
    return best;
}

int job_exec_spawn(uint64_t saved_rsp, int priority)
{
    struct core_state *cs = core_state_get();
    if (cs == NULL) {
        return -1;
    }
    for (int i = 0; i < JOB_EXEC_MAX_RESIDENT; i++) {
        if (cs->job_table[i].state == JOB_EXEC_UNUSED) {
            cs->job_table[i].saved_rsp = saved_rsp;
            cs->job_table[i].priority = priority;
            cs->job_table[i].state = JOB_EXEC_READY;
            cs->job_table[i].waiting_on = NULL;
            return i;
        }
    }
    return -1;
}

static struct core_state *core_state_or_panic(const char *who)
{
    struct core_state *cs = core_state_get();
    if (cs == NULL) {
        /* No correctly-functioning caller reaches here: job_exec_spawn
         * already refuses to hand out a job slot when core_state is
         * NULL, so nothing could have produced a job for this function
         * to be switching into or out of. Loud and stuck beats an
         * unpredictable crash deep inside a context switch. */
        kprintf("[job_exec] PANIC: %s with no core_state\n", who);
        for (;;) {
            arch_cpu_wait_for_interrupt();
        }
    }
    return cs;
}

void job_exec_wait(job_wait_queue_t *q)
{
    struct core_state *cs = core_state_or_panic("job_exec_wait");
    int me = cs->current_job;
    cs->job_table[me].state = JOB_EXEC_BLOCKED;
    cs->job_table[me].waiting_on = q;

    job_exec_slot_t *next = pick_highest_priority_ready(cs);
    if (!next) {
        /* Nothing else ready at all -- Phase A has no real idle job
         * yet to fall back to; this is a genuine "nothing left to run"
         * condition, not something to paper over. */
        for (;;) {
            arch_cpu_wait_for_interrupt();
        }
    }
    int next_idx = (int)(next - cs->job_table);
    cs->job_table[next_idx].state = JOB_EXEC_RUNNING;
    cs->current_job = next_idx;
    job_exec_switch(&cs->job_table[me].saved_rsp, cs->job_table[next_idx].saved_rsp);
    /* Resumes here, transparently, once job_exec_wake(q) picks this
     * slot again and it's actually rescheduled. */
}

void job_exec_wake(job_wait_queue_t *q)
{
    struct core_state *cs = core_state_or_panic("job_exec_wake");
    job_exec_slot_t *woken = NULL;
    for (int i = 0; i < JOB_EXEC_MAX_RESIDENT; i++) {
        if (cs->job_table[i].state == JOB_EXEC_BLOCKED &&
            cs->job_table[i].waiting_on == q) {
            if (!woken || cs->job_table[i].priority > woken->priority) {
                woken = &cs->job_table[i];
            }
        }
    }
    if (!woken) {
        return; /* nobody waiting on q */
    }

    int woken_idx = (int)(woken - cs->job_table);
    cs->job_table[woken_idx].state = JOB_EXEC_READY;
    cs->job_table[woken_idx].waiting_on = NULL;

    int cur = cs->current_job;
    if (cur >= 0 && cs->job_table[woken_idx].priority > cs->job_table[cur].priority) {
        /* The woken job outranks whoever's calling wake() -- preempt
         * immediately, right here. The caller is READY (not BLOCKED --
         * it isn't waiting on anything), so it'll run again as soon as
         * it's next the highest-priority ready job. */
        cs->job_table[cur].state = JOB_EXEC_READY;
        cs->job_table[woken_idx].state = JOB_EXEC_RUNNING;
        cs->current_job = woken_idx;
        job_exec_switch(&cs->job_table[cur].saved_rsp,
                        cs->job_table[woken_idx].saved_rsp);
        /* Resumes here once this job is rescheduled again. */
    }
    /* Otherwise: woken job just sits READY; caller continues normally,
     * no switch happens now. */
}

uint64_t *job_exec_current_rsp_slot(void)
{
    struct core_state *cs = core_state_or_panic("job_exec_current_rsp_slot");
    return &cs->job_table[cs->current_job].saved_rsp;
}

void job_exec_run(uint64_t *save_rsp_to, int slot)
{
    struct core_state *cs = core_state_or_panic("job_exec_run");
    cs->job_table[slot].state = JOB_EXEC_RUNNING;
    cs->current_job = slot;
    job_exec_switch(save_rsp_to, cs->job_table[slot].saved_rsp);
}
