#pragma once

#include <stdint.h>

/* The x86_64 saved-job layout -- an iretq-compatible frame, so the
 * restore path is identical whether the frame came from
 * job_exec_switch's own save sequence, job_exec_init's fake first
 * frame, or (Phase 3) a real hardware interrupt. iretq doesn't know or
 * care who built the frame it consumes; that indifference is the whole
 * mechanism. The aarch64 counterpart lives in job_exec_aarch64.h. */

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
