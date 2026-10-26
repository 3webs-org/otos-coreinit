#pragma once

#include <stdint.h>
#include "cap.h"
#include "job_exec.h"
#include "../include/init_abi.h"

#if defined(__x86_64__)
#define CORE_IRQ_IDT_ENTRIES 256
#define CORE_FAULT_IST_STACK_SIZE 8192

typedef struct {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed)) core_irq_idt_entry_t;

typedef struct {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} __attribute__((packed)) core_irq_tss64_t;

_Static_assert(sizeof(core_irq_idt_entry_t) == 16,
               "x86_64 IDT entries are architecturally 16 bytes");
_Static_assert(sizeof(core_irq_tss64_t) == 104,
               "x86_64 64-bit TSS layout must remain 104 bytes");
#endif

/* Per-core state. The ongoing destination for what used to be a
 * scattered set of globals across job_exec.c, serial_*.c, irq_*.c, the
 * timer backends, job_memory.c, and pagefault_*.c -- all of them
 * formerly safe only because each core ran its own independent
 * byte-for-byte copy of the whole image, which this project is moving
 * away from (see TODO.md / the boot-protocol redesign notes). Migration
 * happens subsystem by subsystem, each verified independently; not
 * every subsystem listed above has moved yet; see TODO.md for the
 * current audit rather than treating this struct as an exhaustive list.
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

    /* coreinit.c's own node-object capability, returned by join_node
     * (cluster_scheduler.c's handle_join_node) once per core at
     * cluster-join time. Genuinely per-core -- each core joins as its
     * own node and gets back its own node object, never shared.
     * cluster_scheduler.c's own preempt_node reaches any node (this one
     * included) by node_id rather than this raw handle, so nothing
     * reads this field yet -- kept for future use, same as every other
     * object handle this project logs and holds onto rather than
     * discarding. Zero-initialized default (no handle) is already
     * correct -- no explicit reset needed in core_state_init_defaults. */
    capability_t my_node;

    /* scheduler.c's client binding is per core under a shared init image:
     * the BSP invokes its local cluster-scheduler object, while an AP invokes
     * a local proxy and has a different node id. These used to be image
     * globals only because every AP received a private image copy. */
    int (*scheduler_invoke)(capability_t, uint32_t, const void *, size_t,
                            void *, size_t, size_t *);
    capability_t scheduler_cluster;
    uint64_t scheduler_node_id;

    /* job_exec.c's own per-core cooperative scheduler: its job table,
     * the job-stack allocation bitmap, which slot is currently running
     * (-1 means none -- see core_state_init_defaults, same reasoning as
     * core_tag above), and the raw stacks themselves. The stacks are
     * embedded directly (JOB_EXEC_MAX_RESIDENT * JOB_EXEC_STACK_SIZE =
     * 512 KiB today) rather than a separate allocation reached by
     * pointer -- deliberately: this is still smaller than the OLD
     * per-core model's fixed 4 MiB CORE_REGION_SIZE reservation (see
     * smp_layout.h), so embedding it costs nothing the old model didn't
     * already spend, and avoids a second allocation with its own
     * failure mode. */
    job_exec_slot_t job_table[JOB_EXEC_MAX_RESIDENT];
    int job_stack_used[JOB_EXEC_MAX_RESIDENT];
    int current_job;
    uint8_t job_stacks[JOB_EXEC_MAX_RESIDENT][JOB_EXEC_STACK_SIZE]
        __attribute__((aligned(16)));

    /* lapic_timer_x86_64.c/timer_aarch64.c's own per-core timer state:
     * THIS core's own tick count and the capability bound to receive its
     * ticks. Genuinely per-core on both architectures -- x86_64's LAPIC
     * timer and aarch64's generic timer both fire independently on
     * every core (unlike x86's legacy-PIC-is-BSP-only case elsewhere).
     * g_timer_freq_hz (aarch64 only) is likewise per-core -- CNTFRQ_EL0
     * is read fresh per core, not assumed shared. */
    uint64_t timer_ticks;
    capability_t timer_target;
    uint64_t timer_freq_hz; /* aarch64 only; unused on x86_64 */

    /* pagefault_x86_64.c/pagefault_aarch64.c's own per-core page-fault
     * state -- genuinely per-core (each core's own page tables, own
     * fault history) on both architectures, but the field SHAPE itself
     * genuinely differs (x86 splits one 2MB region with one table;
     * aarch64 replaces a 1 GiB block with an L2 table, then splits one
     * region with an L3 table, and separately tracks handler
     * readiness since it shares the EL1 vector table with other
     * exception types) -- same precedent pagefault.h itself already
     * uses for the two architectures' hook typedefs. The array sizes
     * below (16, 16, 4) must stay in sync with each file's own
     * MAX_STREAMED_PAGES/MAX_COW_PAGES/COW_POOL_SIZE -- there's no
     * shared header constant to enforce that, since pagefault.h
     * doesn't define one; checked by hand, not by the compiler. */
    /* irq_aarch64.c's own per-core GICv3 state -- resolved, not just
     * unmigrated, after real GICv3-semantics reasoning (see that file's
     * own comments): g_irq_handlers/g_irq_targets stay plain shared
     * globals (every core installs the identical handler for a given
     * vector, and target bindings are a single system-wide policy via
     * the one irq_manager object), but the REDISTRIBUTOR FRAME a core
     * resolves for itself via its own MPIDR affinity is genuinely
     * per-core -- a shared global there would mean one core's discovery
     * silently overwriting another's. irq_tick_count is the aarch64
     * side of a counter x86_64's own irq_x86_64.c still has as a plain
     * global (not touched by this migration -- see TODO.md); kept
     * per-core here to match irq_get_tick_count()'s API contract on
     * this architecture even though nothing on aarch64 reads it today.
     * x86_64's corresponding per-core descriptor tables and fault IST
     * live in the architecture block below. */
#if defined(__aarch64__)
    uint64_t mmu_l1[512] __attribute__((aligned(4096)));
    int mmu_owned_map;
    volatile uint8_t *irq_gicr_base;
    uint64_t irq_tick_count;
#endif

#if defined(__x86_64__)
    core_irq_idt_entry_t irq_idt[CORE_IRQ_IDT_ENTRIES];
    uint8_t irq_fault_ist_stack[CORE_FAULT_IST_STACK_SIZE]
        __attribute__((aligned(16)));
    core_irq_tss64_t irq_tss;
    uint64_t irq_gdt_with_tss[7] __attribute__((aligned(16)));

    uint64_t pf_split_pt[512] __attribute__((aligned(4096)));
    uint64_t pf_split_region_base;
    int pf_have_split;
    uint64_t pf_streamed_pages[16];
    size_t pf_streamed_count;
    uint64_t pf_cow_pages[16];
    size_t pf_cow_count;
    uint8_t pf_cow_pool[4][4096] __attribute__((aligned(4096)));
    size_t pf_cow_pool_used;
    uint64_t pf_fault_count;
    uint64_t pf_cow_copy_count;
#elif defined(__aarch64__)
    uint64_t pf_l2[512] __attribute__((aligned(4096)));
    uint64_t pf_l3[512] __attribute__((aligned(4096)));
    uint64_t pf_split_base;
    int pf_have_split;
    int pf_handler_ready;
    uint64_t pf_streamed[16];
    uint64_t pf_cow[16];
    size_t pf_streamed_count;
    size_t pf_cow_count;
    size_t pf_cow_used;
    uint8_t pf_cow_pool[4][4096] __attribute__((aligned(4096)));
    uint64_t pf_fault_count;
    uint64_t pf_cow_copies;
#endif
};

/* Sets every field whose correct "not yet set" value is NOT the
 * zero-initialized default (core_tag, current_job: both use -1 for
 * "none", since 0 is a real core/slot index) -- called once per core,
 * immediately after that core's own core_state is obtained (both the
 * BSP's static instance and an AP's freshly bump-allocated one), before
 * core_state_set makes it reachable. A field whose correct default
 * genuinely IS zero (every other field above) needs nothing here. */
void core_state_init_defaults(struct core_state *cs);

/* Set THIS core's own pointer. Called once per core, early in its own
 * entry -- coreinit_main for the BSP, immediately on entry for an AP. */
void core_state_set(struct core_state *cs);

/* Get THIS core's own pointer, as set above. NULL if core_state_set was
 * never called on this core (e.g. an AP that couldn't be given one -- see
 * core_state_alloc_ap). Callers must check; there is no fallback value. */
struct core_state *core_state_get(void);

/* Allocates and zero-initializes one struct core_state through the boot
 * allocator recorded in info->init_description_phys (see
 * init_abi.h and boot_protocol.h). Returns NULL if the allocator is
 * absent, malformed, finalized, or exhausted -- non-fatal: nothing yet
 * depends on every core having one of these wired up, so a caller that
 * gets NULL back simply has no per-core state this boot, the same as if
 * this mechanism didn't exist yet. */
struct core_state *core_state_alloc_ap(const struct init_boot_info *info);
