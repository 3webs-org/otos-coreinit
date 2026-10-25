#pragma once

#include <stdint.h>
#include "cap.h"
#include "../include/init_abi.h"

/* Per-core state. The planned destination for what today is a scattered
 * set of globals across job_exec.c, serial_*.c, irq_*.c, the timer
 * backends, job_memory.c, and pagefault_*.c -- all of them currently safe
 * only because each core runs its own independent byte-for-byte copy of
 * the whole image, which this project is moving away from (see TODO.md /
 * the boot-protocol redesign notes). Migration happens subsystem by
 * subsystem; this struct is deliberately a stub for now -- nothing reads
 * or writes a real field here yet. This file only wires up the mechanism
 * (role detection, the per-core pointer register, the AP allocator) with
 * zero behavior change elsewhere.
 *
 * Reached via a dedicated per-core pointer register (TPIDR_EL1 on
 * aarch64, a dedicated MSR on x86_64) rather than threaded through every
 * function signature that would otherwise need it -- see
 * core_state_set/core_state_get below. Same split as arch_mmu.h/
 * cross_core_signal.h: this header is arch-neutral, exactly one .c file
 * per architecture implements the register access. */
struct core_state {
    uint64_t core_id;

    /* serial_x86_64.c/serial_aarch64.c's own per-core log tag, so
     * interleaved output from multiple simultaneously-running cores can
     * be attributed. -1 means "not yet set" (prints untagged, e.g.
     * before a core knows its own ID) -- NOT the zero-initialized
     * default, so whoever sets up a core_state (coreinit_main, for both
     * the BSP's static instance and an AP's freshly-allocated one) must
     * explicitly set this to -1 before anything can log; a zero left as
     * the implicit default would misreport as "core 0" instead of
     * "untagged". */
    int core_tag;

    /* job_memory.c's own PML4 physical address for THIS core -- used to
     * fault-handle a non-owned STREAM/COW resource locally. Was
     * job_memory.c's own file-scope global; moved here first since it's
     * a lone scalar with one writer (job_memory_init) and reads confined
     * to job_memory_acquire_resource. */
    uint64_t my_pml4_phys;

    /* coreinit.c's cross-core-signal self-test flag for THIS core. Despite
     * the name, this is a real functional self-test that runs on every
     * core's own boot today (job_timer_bringup, not BSP-gated): each core
     * signals itself and waits for its own ISR to set this back. Left as
     * a bare global it would be a genuine cross-core race under shared
     * code (two cores corrupting each other's result). */
    volatile int cross_core_signal_test_seen;

    /* coreinit.c's per-core capability/proxy handles reaching the ONE
     * shared hardware mutex (guarding legacy PIC/PIT access). Each core
     * gets its own handle to the same shared object -- the handles are
     * per-core, not the object they name. */
    capability_t hw_mutex_manager;
    uint64_t hw_mutex_id;
};

/* Set THIS core's own pointer. Called once per core, early in its own
 * entry -- coreinit_main for the BSP, immediately on entry for an AP. */
void core_state_set(struct core_state *cs);

/* Get THIS core's own pointer, as set above. NULL if core_state_set was
 * never called on this core (e.g. an AP that couldn't be given one -- see
 * core_state_alloc_ap). Callers must check; there is no fallback value. */
struct core_state *core_state_get(void);

/* Bump-allocates and zero-initializes one struct core_state from the
 * free-memory pool handed to init at boot (info->free_memory_base/len --
 * see init_abi.h's own note on that field). Must be called with the
 * SAME info on every call from a single core's boot sequence (it seeds
 * the pool from info the first time it's invoked; later calls ignore
 * info and just keep bumping). Returns NULL if the pool is absent or
 * exhausted -- non-fatal: nothing yet depends on every core having one of
 * these wired up, so a caller that gets NULL back simply has no per-core
 * state this boot, the same as if this mechanism didn't exist yet. */
struct core_state *core_state_alloc_ap(const struct init_boot_info *info);
