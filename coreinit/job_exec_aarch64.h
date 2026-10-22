#pragma once

#include <stdint.h>

/* The aarch64 saved-job layout -- x0-x30 plus sp plus the saved DAIF,
 * matched by hand against job_exec_switch_aarch64.S, same
 * no-type-system discipline as the x86_64 pair in job_exec_x86_64.h.
 * The x86_64 frame's rip is x30 here (the resume PC, since AAPCS gives
 * no hardware push of a return address on interrupt); the x86_64
 * frame's rflags is pstate here, restored by the load path the way
 * iretq's atomic RFLAGS restore does. */

/* The exact layout of a job's saved execution context, as it sits at
 * the address job_exec_switch wrote into *save_to -- matching EXACTLY
 * what job_exec_switch_aarch64.S's save/restore sequence stores and
 * loads, in this order. Getting this struct's field order wrong
 * relative to the actual store/load offsets is silent, catastrophic
 * corruption of a job's register state -- there is no type system
 * connecting a raw assembly stp/ldr pair to this struct's field order,
 * only this comment and matching them by hand, carefully, on both
 * sides, forever. */
typedef struct __attribute__((packed)) {
    uint64_t x0, x1, x2, x3, x4, x5, x6, x7;
    uint64_t x8, x9, x10, x11, x12, x13, x14, x15;
    uint64_t x16, x17;
    uint64_t x18;      /* the platform-reserved register, saved anyway */
    uint64_t x19, x20, x21, x22, x23, x24, x25, x26, x27, x28;
    uint64_t x29;      /* frame pointer */
    uint64_t x30;      /* link register -- THE RESUME PC, the ARM
                        * analogue of the x86 frame's rip */
    uint64_t sp;       /* this job's own stack pointer once running */
    uint64_t pstate;   /* the saved DAIF, so the resuming job gets its
                        * OWN interrupt state back -- the analogue of
                        * iretq's atomic RFLAGS restore */
} job_saved_context_aarch64_t;

/* DAIF (bits 9:6) are MASKS -- a set bit means that class of exception
 * is masked, the inverse of x86's IF where set means enabled. All-zero
 * therefore means IRQs and FIQs ENABLED, which is what a freshly
 * created job should run with: the same intent, and the same numeric
 * value, as x86's JOB_EXEC_INITIAL_RFLAGS 0x202 (whose bit 9 is the
 * enabled-interrupt flag). */
#define JOB_EXEC_INITIAL_PSTATE 0x0ull
