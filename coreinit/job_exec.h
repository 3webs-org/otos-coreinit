#pragma once

#include <stddef.h>
#include <stdint.h>

/* The exact layout of a job's saved execution context, as it sits on
 * its own stack after a save (whether from a real interrupt, or
 * job_exec_init's own fake first frame) -- read starting at a
 * saved_rsp value, matching EXACTLY what job_exec_switch's save/
 * restore sequence pushes and pops, in this order. Getting this
 * struct's field order wrong relative to the actual push/pop sequence
 * is silent, catastrophic stack corruption -- there is no type system
 * connecting a raw assembly push/pop pair to this struct's field
 * order, only this comment and matching them by hand, carefully, on
 * both sides, forever. */
typedef struct __attribute__((packed)) {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rbx, rdx, rcx, rax;
    /* The CPU's own interrupt frame -- pushed by hardware on a real
     * interrupt (Phase 3), or laid out by hand here for a job's very
     * first run. iretq in 64-bit mode always pops all five of these,
     * unconditionally, even with no privilege-level change -- verified
     * against the Intel SDM directly before relying on it, since a
     * wrong assumption here means silent stack corruption on every
     * single switch. */
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} job_saved_context_t;

/* Segment selectors from this project's own GDT (kernel/boot.S) --
 * duplicated here by necessity, same reasoning as init_abi.h's own
 * note on why its struct fields are duplicated rather than shared
 * (coreinit and the kernel are compiled and linked completely
 * independently). */
#define JOB_EXEC_CODE_SELECTOR 0x08ull
#define JOB_EXEC_DATA_SELECTOR 0x10ull

/* RFLAGS with interrupts enabled (bit 9) and the always-set reserved
 * bit 1 -- what a freshly-created job should run with. */
#define JOB_EXEC_INITIAL_RFLAGS 0x202ull

/* Constructs a fake "already saved" context at the top of a fresh
 * stack, so the very first switch INTO this job looks, to the restore
 * code, identical to resuming a job that really was interrupted
 * mid-execution -- the job "begins" at entry(arg) as if it had just
 * been interrupted at its own first instruction, before it ever ran
 * anything. Returns the initial saved_rsp value to store in the job's
 * own bookkeeping -- NOT the raw stack_top passed in, which is
 * deliberately higher than where the returned value points (the
 * constructed frame lives just below stack_top, since a stack grows
 * down). */
uint64_t job_exec_init(void *stack_top, void (*entry)(uint64_t arg),
                       uint64_t arg);

/* A small, fixed, per-core pool of raw job stacks -- local bookkeeping
 * only, never reachable via kernel_invoke (unlike a job's identity and
 * scheduling state, which stay real cluster-wide objects in
 * cluster_scheduler.c; a job's REGISTER STATE is only ever meaningful
 * on the one core actually running it). */
#define JOB_EXEC_MAX_RESIDENT 16
#define JOB_EXEC_STACK_SIZE 0x8000ull /* 32 KiB -- generous for
                                       * demo-scale code, not tuned */

/* Claims a free stack slot on THIS core. Returns the stack's TOP
 * address (highest address -- stacks grow down) via *out_top, and the
 * slot index (for job_exec_free_stack later) via *out_slot; *out_slot
 * is set to -1 and *out_top to 0 if the pool is exhausted. */
void job_exec_alloc_stack(uint64_t *out_top, int *out_slot);

void job_exec_free_stack(int slot);

/* --- A real per-core job table, and the wait/wake primitives that
 * make blocking mean something instead of stalling forever. ---
 *
 * A job doesn't just yield -- it yields ONTO something (a mutex, a
 * resource, a condvar), represented here as a job_wait_queue_t. Only
 * the thing being waited on can ever wake it; nothing scans "ready
 * jobs" looking for it, because a blocked job isn't in that set at
 * all. Every block has an owner responsible for the wake, the same
 * way every reservation in this project has an owner responsible for
 * releasing it.
 *
 * job_wait_queue_t carries no state of its own here -- its identity
 * (its own address) is the token a blocked job's slot records and a
 * waking call searches for. One instance per mutex/resource/condvar,
 * same as this project's other one-object-per-thing pattern. */
typedef struct {
    int unused_marker; /* exists so distinct queues have distinct
                        * addresses; nothing reads this field */
} job_wait_queue_t;

typedef enum {
    JOB_EXEC_UNUSED = 0,
    JOB_EXEC_READY,
    JOB_EXEC_RUNNING,
    JOB_EXEC_BLOCKED,
} job_exec_state_t;

/* A job's LOCAL, per-core bookkeeping -- priority and blocking state,
 * as distinct from cluster_scheduler.c's own job_data_t (that one is
 * the cross-node-visible identity and scheduling metadata; this one
 * only ever matters to the core actually running the job, same
 * reasoning as job_saved_context_t living here rather than there). */
typedef struct {
    uint64_t saved_rsp;
    int priority;
    job_exec_state_t state;
    int stack_slot;
    const job_wait_queue_t *waiting_on; /* meaningful only when state
                                        * == JOB_EXEC_BLOCKED */
} job_exec_slot_t;

/* Registers a freshly job_exec_init'd job in this core's own table,
 * READY to run. Returns its slot index, or -1 if the table is full. */
int job_exec_spawn(uint64_t saved_rsp, int priority);

/* Called BY the currently running job to block itself on q and switch
 * away to whatever the highest-priority READY job is. Resumes here,
 * transparently, once job_exec_wake(q) later picks this job and it's
 * actually rescheduled -- the caller's own stack, locals, and position
 * in its own code are exactly as they were, the entire point of this
 * being a real execution context rather than a restart-from-scratch
 * callback. */
void job_exec_wait(job_wait_queue_t *q);

/* Called by whoever releases/signals q (typically from inside a
 * currently-running job's own code, e.g. right after clearing a
 * mutex's locked flag). Wakes the highest-priority job blocked on q,
 * if any. If that job now outranks whatever's CURRENTLY running, this
 * preempts immediately, right here -- the caller itself gets switched
 * away from involuntarily (marked READY, not BLOCKED, since it isn't
 * waiting on anything) and only runs again once it's next scheduled.
 * If the woken job doesn't outrank the current one, it's simply marked
 * READY and this returns normally with no switch. Does nothing if
 * nothing is waiting on q. */
void job_exec_wake(job_wait_queue_t *q);

/* Switches into a spawned job for the very first time, correctly
 * setting g_current_job so its own later job_exec_wait/wake calls have
 * a valid "which slot am I" to work with -- job_exec_switch alone
 * (Phase 2's raw primitive) does NOT touch g_current_job, so using it
 * directly for an initial kickoff leaves g_current_job at its previous
 * value (likely -1, a non-job context), corrupting the very next
 * job_exec_wait call's g_job_table[-1] access. save_rsp_to is where
 * the CALLER's own context (not itself a spawned job) is saved. */
void job_exec_run(uint64_t *save_rsp_to, int slot);

/* Returns a pointer to the CURRENTLY running job's own saved_rsp slot
 * -- lets external code (a test driver, say) do one final explicit
 * job_exec_switch back to whatever context originally spawned it,
 * without needing direct access to the job table's own internals. */
uint64_t *job_exec_current_rsp_slot(void);

/* The actual switch primitive -- see job_exec_switch.S for the full
 * design. Saves the calling job's complete register state to its own
 * stack and writes the resulting saved_rsp to *save_rsp_to, then loads
 * and resumes execution at load_rsp_from (another job's own saved_rsp
 * value). Does not return in the ordinary sense to ITS caller -- by
 * the time this "returns," a DIFFERENT job is running; the original
 * caller only runs again once something switches back to it, at which
 * point execution resumes exactly here, as if this call had returned
 * normally. */
void job_exec_switch(uint64_t *save_rsp_to, uint64_t load_rsp_from);
