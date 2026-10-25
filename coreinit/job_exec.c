#include "job_exec.h"
#include "arch_cpu.h"

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

/* --- Per-core job table and the wait/wake scheduler --- */

/* The initial-frame builder is architecture-specific -- see
 * job_exec_x86_64.c / job_exec_aarch64.c, selected per-arch by
 * CMakeLists.txt. Everything below this point is arch-neutral
 * bookkeeping that only ever handles the opaque saved_rsp value that
 * builder (or the switch primitive) produced. */

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
            arch_cpu_wait_for_interrupt();
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
