/* coreinit: stage 1, built on top of the one syscall (kernel_invoke)
 * rather than providing it. See cap.h for the kernel's own side of this
 * contract.
 *
 * Stage boundary: coreinit's job is bringing up devices and delivering
 * real hardware to the object model (PCI discovery, disk I/O, interrupt
 * handling) -- nothing here does or should do authorization. Wrapping
 * objects for access control (the auth/call-stack pattern this project
 * proved out earlier) is explicitly OUT of scope for this stage: that's
 * securinit's job, a later stage coreinit will eventually load and hand
 * off to, the same one-shot way the kernel handed off to coreinit
 * itself. Every object created here is deliberately unguarded; nothing
 * about that is an oversight to fix later in *this* file.
 *
 * This file is compiled as a Mach-O binary, not ELF, targeting
 * <arch>-apple-darwin for whichever toolchain file was selected; it
 * includes no architecture-specific header.
 */

#include "topology.h"
#include "ata.h"
#include "cap.h"
#include "capnp_build.h"
#include "generated_schemas.h"
#include "securinit-generated/generated_schemas.h"
#include "cluster_scheduler.h"
#include "arch_mmu.h"
#include "core_state.h"
#include "cross_core_signal.h"
#include "irq.h"
#include "timer.h"
#include "arch_cpu.h"
#include "job_memory.h"
#include "job_exec.h"
#include "mem_mgr.h"
#include "mutex_mgr.h"
#include "pagefault.h"
#include "pci.h"
#include "proxy.h"
#include "scheduler.h"
#include "serial.h"
#include "smp.h"
#include "stage_loader.h"
#include "../include/securinit_abi.h"
#include "../include/init_abi.h"
#include "../include/smp_layout.h"

/* NOTE on mode64_token: see kernel/arch_x86_64.h's own comment for why
 * the kernel's copy of those primitives is gated. This file includes no
 * architecture-specific header at all -- nothing in it needs one. */

extern const uint8_t securinit_image_start[];
extern const uint8_t securinit_image_end[];

#if defined(__x86_64__)
static uint64_t g_boot_pml4_phys;
#endif

static int (*g_kernel_invoke)(capability_t target, uint32_t method_id,
                              const void *msg, size_t len, void *response,
                              size_t response_cap, size_t *out_response_len);

/* The factory (object-creation) object's own handle -- handed over
 * explicitly in info->caps.factory (see coreinit_main below and
 * include/init_abi.h's own note on why this is no longer a
 * well-known constant). Threaded into proxy_init/
 * cluster_scheduler_init_schema the same way g_kernel_invoke already
 * is, for the same reason: each file that needs it keeps its own
 * static copy rather than sharing one global across translation
 * units. */
static capability_t g_factory;

/* The shared "serial" resource -- reserved briefly by job_kprintf
 * around each print, so two cores' output can't interleave mid-line.
 * See job_kprintf's own doc comment. Declared here, ahead of
 * job_kprintf's own use of it. */
static capability_t g_serial_resource;

/* ---- Thin helpers over the factory object's own protocol ---- */

/* Storage for objects created via create_object below -- a small,
 * fixed pool because these are ALL created once, early, during
 * bootstrap, in a small, known quantity (irq_manager, tick_counter,
 * disk, mutex_manager, memory_manager, cluster_scheduler, and a couple
 * more as this project grows) -- not a reintroduction of the kernel's
 * old fixed ceiling, since the kernel itself no longer has or enforces
 * one at all (see cap.h). Anything that actually churns at runtime
 * (job or resource objects, were they to become real capability
 * objects) should draw its storage from mem_mgr's own allocator
 * instead, which is genuinely unbounded. */
#define BOOTSTRAP_RECORD_POOL_SIZE 32
static uint8_t g_bootstrap_records[BOOTSTRAP_RECORD_POOL_SIZE][CAP_RECORD_SIZE]
    __attribute__((aligned(CAP_RECORD_ALIGN)));
static size_t g_bootstrap_record_next;

static capability_t create_object(object_handler_fn handler, void *data,
                                  const schema_table_t *table,
                                  schema_idx_t schema_idx)
{
    if (g_bootstrap_record_next >= BOOTSTRAP_RECORD_POOL_SIZE) {
        kprintf("[coreinit] create_object: FAILED, bootstrap record pool "
                "exhausted\n");
        capability_t none = {0};
        return none;
    }
    void *record_storage = g_bootstrap_records[g_bootstrap_record_next++];

    /* CREATE's request shape is CAP_CREATE_REQUEST_WORDS data words:
     * handlerFn, handlerData, schemaTable, schemaIndex, recordStorage --
     * the exact order cap.c's factory_handler unpacks them in. The
     * schemaIndex rides in a u64 word but is a uint16_t index into the
     * table, not a pointer: integer initialisers need no relocation, so
     * the table bytes are identical for every kernel instance. */
    uint64_t words[5] = {(uint64_t)(uintptr_t)handler,
                         (uint64_t)(uintptr_t)data,
                         (uint64_t)(uintptr_t)table,
                         (uint64_t)schema_idx,
                         (uint64_t)(uintptr_t)record_storage};
    uint8_t req[64];
    size_t req_len = capnp_build_flat(req, sizeof(req), words, 5);

    uint8_t resp[24];
    size_t resp_len = 0;
    int ok = g_kernel_invoke(g_factory, 0, req, req_len, resp, sizeof(resp),
                             &resp_len);
    if (!ok || resp_len < 24) {
        kprintf("[coreinit] create_object: FAILED\n");
        capability_t none = {0};
        return none;
    }
    const uint64_t *out = capnp_read_flat(resp);
    capability_t result;
    result.handle = out[0];
    return result;
}

/* Prints via kprintf, but first reserves the shared "serial" resource
 * briefly (for the duration of this one call) so a concurrently
 * printing job on another core can't interleave with this one mid-line
 * -- the corruption this was built to fix wasn't cosmetic: it was
 * genuinely garbled bytes spliced into unrelated output. If the
 * reserve fails (someone else holds it right now), this message is
 * simply skipped rather than printed anyway -- printing regardless
 * would defeat the point, since the contended case is exactly when two
 * cores would actually be mid-print at once. Contention should be rare
 * and brief; an occasionally dropped debug line is a far better trade
 * than either corrupted output or a lock held across the slow,
 * byte-at-a-time UART transmission each print involves. Not usable
 * outside a job's own context -- self_job_id has to mean something to
 * the resource's own RESERVE method. */
static void job_kprintf(uint64_t self_job_id, const char *fmt, ...)
{
    if (g_serial_resource.handle == 0) {
        /* Not set up yet (very early boot) -- fall back to a plain,
         * unprotected print rather than silently dropping everything
         * before the resource exists. */
        va_list ap;
        va_start(ap, fmt);
        kvprintf(fmt, ap);
        va_end(ap);
        return;
    }

    uint64_t words[1] = {self_job_id};
    uint8_t req[24];
    size_t req_len = capnp_build_flat(req, sizeof(req), words, 1);
    uint8_t resp2[24];
    size_t resp2_len = 0;
    int reserved =
        g_kernel_invoke(g_serial_resource, RESOURCE_METHOD_RESERVE, req,
                        req_len, resp2, sizeof(resp2), &resp2_len) &&
        resp2_len >= 24 && capnp_read_flat(resp2)[0] != 0;
    if (!reserved) {
        return; /* someone else is mid-print right now -- skip this line
                 * rather than risk interleaving with it */
    }

    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);

    g_kernel_invoke(g_serial_resource, RESOURCE_METHOD_RELEASE_RESERVATION,
                    req, req_len, resp2, sizeof(resp2), &resp2_len);
}

/* --- Phase 2 isolated test: does job_exec_switch actually work? ---
 * Deliberately independent of cluster_scheduler/the job system
 * entirely -- this tests ONLY the raw mechanism (job_exec_init's fake
 * frame, job_exec_switch's save/restore, iretq resuming correctly) in
 * complete isolation, so a bug here shows up immediately and
 * unambiguously rather than being buried inside scheduler complexity.
 * Two jobs ping-pong a fixed number of times, then one switches back
 * to this driver, which resumes exactly where it left off if (and
 * only if) the whole mechanism is correct. */
static uint64_t g_test_driver_rsp, g_test_job_a_rsp, g_test_job_b_rsp;
static int g_test_switch_count;

static void job_exec_test_b(uint64_t arg);

static void job_exec_test_a(uint64_t arg)
{
    (void)arg;
    for (int i = 0; i < 3; i++) {
        kprintf("[coreinit]   job_exec test: A iteration %d\n", i);
        g_test_switch_count++;
        job_exec_switch(&g_test_job_a_rsp, g_test_job_b_rsp);
    }
    kprintf("[coreinit]   job_exec test: A done, switching back to the "
            "driver\n");
    job_exec_switch(&g_test_job_a_rsp, g_test_driver_rsp);
    for (;;) {
        arch_cpu_wait_for_interrupt();
    }
}

static void job_exec_test_b(uint64_t arg)
{
    (void)arg;
    for (int i = 0; i < 3; i++) {
        kprintf("[coreinit]   job_exec test: B iteration %d\n", i);
        g_test_switch_count++;
        job_exec_switch(&g_test_job_b_rsp, g_test_job_a_rsp);
    }
    kprintf("[coreinit]   job_exec test: B done\n");
    for (;;) {
        arch_cpu_wait_for_interrupt();
    }
}

static void run_job_exec_test(void)
{
    kprintf("\n[coreinit] === job_exec Phase 2 test: two jobs ping-ponging "
            "via real context switches ===\n");

    uint64_t stack_a_top, stack_b_top;
    int slot_a, slot_b;
    job_exec_alloc_stack(&stack_a_top, &slot_a);
    job_exec_alloc_stack(&stack_b_top, &slot_b);
    if (slot_a < 0 || slot_b < 0) {
        kprintf("[coreinit]   job_exec test: stack allocation failed\n");
        return;
    }

    g_test_job_a_rsp =
        job_exec_init((void *)(uintptr_t)stack_a_top, job_exec_test_a, 0);
    g_test_job_b_rsp =
        job_exec_init((void *)(uintptr_t)stack_b_top, job_exec_test_b, 0);

    kprintf("[coreinit]   job_exec test: switching into job A for the "
            "first time\n");
    job_exec_switch(&g_test_driver_rsp, g_test_job_a_rsp);

    kprintf("[coreinit]   job_exec test: back in the driver -- resumed "
            "correctly. switches observed: %d (expect 6)\n",
            g_test_switch_count);
    kprintf("[coreinit]   job_exec test: %s\n",
            (g_test_switch_count == 6) ? "PASS" : "FAIL");

    job_exec_free_stack(slot_a);
    job_exec_free_stack(slot_b);
}

/* --- Phase B test: does wait/wake priority preemption actually work?
 * ---
 * Three jobs, three priorities. The thing this actually proves isn't
 * "all three ran" -- it's the ORDER: HIGH must resume (preempting LOW
 * mid-wake-call) BEFORE LOW's own "background work" line prints. A
 * naive round-robin or a wake that doesn't check priority would still
 * print all five lines, just in the wrong order -- so the test checks
 * the recorded sequence exactly, not just that each line appeared. */
#define EVT_HIGH_BLOCKED 1
#define EVT_MED_RAN 2
#define EVT_LOW_ABOUT_TO_WAKE 3
#define EVT_HIGH_RESUMED 4
#define EVT_LOW_BACKGROUND 5

#define MAX_TEST_EVENTS 16
static int g_test_events[MAX_TEST_EVENTS];
static int g_test_event_count;

static void record_test_event(int code)
{
    if (g_test_event_count < MAX_TEST_EVENTS) {
        g_test_events[g_test_event_count++] = code;
    }
}

static job_wait_queue_t g_test_resource_q;
static job_wait_queue_t g_test_med_done_q;
static job_wait_queue_t g_test_high_done_q;
static uint64_t g_priority_test_driver_rsp;

static void job_exec_priority_test_high(uint64_t arg)
{
    (void)arg;
    record_test_event(EVT_HIGH_BLOCKED);
    job_exec_wait(&g_test_resource_q);
    record_test_event(EVT_HIGH_RESUMED);
    job_exec_wait(&g_test_high_done_q); /* park forever -- nobody wakes
                                        * this; just gets HIGH out of
                                        * the way once it's done */
    for (;;) {
        arch_cpu_wait_for_interrupt();
    }
}

static void job_exec_priority_test_med(uint64_t arg)
{
    (void)arg;
    record_test_event(EVT_MED_RAN);
    job_exec_wait(&g_test_med_done_q); /* park forever */
    for (;;) {
        arch_cpu_wait_for_interrupt();
    }
}

static void job_exec_priority_test_low(uint64_t arg)
{
    (void)arg;
    record_test_event(EVT_LOW_ABOUT_TO_WAKE);
    job_exec_wake(&g_test_resource_q);
    /* If wake correctly preempted, everything above this point ran
     * BEFORE HIGH resumed; everything below only runs once LOW is
     * rescheduled again, AFTER HIGH parks itself. */
    record_test_event(EVT_LOW_BACKGROUND);
    job_exec_switch(job_exec_current_rsp_slot(), g_priority_test_driver_rsp);
    for (;;) {
        arch_cpu_wait_for_interrupt();
    }
}

static void run_job_exec_priority_test(void)
{
    kprintf("\n[coreinit] === job_exec priority preemption test: LOW(1), "
            "MED(5), HIGH(10) ===\n");
    g_test_event_count = 0;

    uint64_t stack_high, stack_med, stack_low;
    int slot_high, slot_med, slot_low;
    job_exec_alloc_stack(&stack_high, &slot_high);
    job_exec_alloc_stack(&stack_med, &slot_med);
    job_exec_alloc_stack(&stack_low, &slot_low);
    if (slot_high < 0 || slot_med < 0 || slot_low < 0) {
        kprintf("[coreinit]   priority test: stack allocation failed\n");
        return;
    }

    uint64_t high_rsp = job_exec_init((void *)(uintptr_t)stack_high,
                                      job_exec_priority_test_high, 0);
    uint64_t med_rsp = job_exec_init((void *)(uintptr_t)stack_med,
                                     job_exec_priority_test_med, 0);
    uint64_t low_rsp = job_exec_init((void *)(uintptr_t)stack_low,
                                     job_exec_priority_test_low, 0);

    int slot_high_job = job_exec_spawn(high_rsp, 10);
    job_exec_spawn(med_rsp, 5);
    job_exec_spawn(low_rsp, 1);

    kprintf("[coreinit]   priority test: switching into HIGH (highest "
            "priority ready job) first\n");
    job_exec_run(&g_priority_test_driver_rsp, slot_high_job);

    kprintf("[coreinit]   priority test: back in the driver. event "
            "sequence: ");
    for (int i = 0; i < g_test_event_count; i++) {
        kprintf("%d ", g_test_events[i]);
    }
    kprintf("(expect 1 2 3 4 5)\n");

    int expected[5] = {EVT_HIGH_BLOCKED, EVT_MED_RAN, EVT_LOW_ABOUT_TO_WAKE,
                       EVT_HIGH_RESUMED, EVT_LOW_BACKGROUND};
    int pass = (g_test_event_count == 5);
    for (int i = 0; pass && i < 5; i++) {
        if (g_test_events[i] != expected[i]) {
            pass = 0;
        }
    }
    kprintf("[coreinit]   priority test: %s\n", pass ? "PASS" : "FAIL");

    job_exec_free_stack(slot_high);
    job_exec_free_stack(slot_med);
    job_exec_free_stack(slot_low);
}


#define PLAIN_SCHEMA_TICK_COUNTER 0

/* ---- Device discovery ---- */

static const char *pci_class_name(uint8_t class_code)
{
    switch (class_code) {
    case 0x00: return "unclassified";
    case 0x01: return "mass storage controller";
    case 0x02: return "network controller";
    case 0x03: return "display controller";
    case 0x04: return "multimedia controller";
    case 0x05: return "memory controller";
    case 0x06: return "bridge";
    case 0x07: return "simple communication controller";
    case 0x08: return "base system peripheral";
    case 0x09: return "input device controller";
    case 0x0C: return "serial bus controller";
    default: return "unrecognized class";
    }
}

static void print_pci_device(const pci_device_info_t *dev, void *user_data)
{
    (void)user_data;
    kprintf("[coreinit]   %u:%u.%u  vendor 0x%X device 0x%X  class 0x%X "
            "(%s)\n",
            dev->bus, dev->device, dev->function, dev->vendor_id,
            dev->device_id, dev->class_code, pci_class_name(dev->class_code));
}

/* ---- Disk object: a real device capability, validated the same way as
 * everything else. Method 0 = read one sector by LBA. Deliberately
 * unguarded -- see file header. ---- */


#define DISK_SCHEMA_DISK 0

static void disk_handler(void *data, uint32_t method_id, const void *msg,
                         size_t len, void *response, size_t response_cap,
                         size_t *out_response_len)
{
    (void)data;
    (void)method_id;
    (void)len;
    *out_response_len = 0;

    const uint64_t *req = capnp_read_flat(msg);
    uint32_t lba = (uint32_t)req[0];

    uint64_t sector_words[64];
    if (!ata_read_sector(lba, sector_words)) {
        kprintf("[coreinit]   disk: read of LBA %u failed\n", (unsigned)lba);
        return;
    }
    *out_response_len =
        capnp_build_flat(response, response_cap, sector_words, 64);
    kprintf("[coreinit]   disk: read LBA %u successfully\n", (unsigned)lba);
}

/* ---- Tick counter: a real object, invoked from real hardware interrupt
 * context (IRQ0, the PIT), bound through irq_manager the same way any
 * other object relationship in this system is established -- create it,
 * hand its capability to whoever should reach it, done. Also
 * deliberately unguarded. ---- */

static uint32_t g_ticks_seen = 0;

static void tick_counter_handler(void *data, uint32_t method_id,
                                 const void *msg, size_t len, void *response,
                                 size_t response_cap, size_t *out_response_len)
{
    (void)data;
    (void)method_id;
    (void)response;
    (void)response_cap;
    (void)len;
    *out_response_len = 0;
    const uint64_t *words = capnp_read_flat(msg);
    (void)words; /* the IRQ number itself; unused here, just counting */
    g_ticks_seen++;
    if (g_ticks_seen % 500 == 0) {
        kprintf("[coreinit]   tick_counter: %u interrupts delivered as real "
                "object invocations\n",
                (unsigned)g_ticks_seen);
    }
}

/* ---- The hardware mutex: guards genuinely shared, non-SMP-safe legacy
 * hardware (the PIC, the PIT) that every core's own irq_core_init() would
 * otherwise reconfigure independently with zero arbitration. Reaching
 * it is either an ordinary local invoke (BSP, which hosts it directly)
 * or a call through the BSP's OWN kernel_invoke at its own address
 * (AP, via the discovery record) -- the lock/unlock call sites below
 * don't need to know or care which. ---- */

static int (*g_hw_mutex_invoke)(capability_t, uint32_t, const void *, size_t,
                                void *, size_t, size_t *);

/* g_hw_mutex_manager/g_hw_mutex_id (the capability handle and mutex ID --
 * per-core, see core_state.h) now live in struct core_state.
 * g_hw_mutex_invoke stays a plain global: it's always this core's own
 * local kernel_invoke regardless of role (see the two assignment sites
 * below), not a per-core value in the same sense. */

static void hw_mutex_lock(void)
{
    struct core_state *cs = core_state_get();
    if (cs == NULL) {
        /* No core_state means no recorded handle to lock through --
         * unlike the other core_state fields, skipping this is NOT
         * safe (the caller proceeds believing it holds exclusive
         * access to shared PIC/PIT hardware when it does not). This
         * can only happen on an AP before a real bootloader supplies
         * the free-memory pool core_state_alloc_ap needs -- loud and
         * stuck is better than silently racing real hardware. */
        kprintf("[coreinit] PANIC: hw_mutex_lock with no core_state\n");
        for (;;) {
            arch_cpu_wait_for_interrupt();
        }
    }
    uint8_t req[24];
    size_t req_len = capnp_build_flat(req, sizeof(req), &cs->hw_mutex_id, 1);
    /* METHOD_LOCK is try-only (see mutex_mgr.c) -- it never blocks
     * inside the handler, so contention is handled HERE, by actually
     * yielding the core (arch_cpu_wait_for_interrupt, woken by the
     * next interrupt) between attempts rather than spinning. */
    for (;;) {
        uint8_t resp[24];
        size_t resp_len = 0;
        if (g_hw_mutex_invoke(cs->hw_mutex_manager, 1, req, req_len, resp,
                              sizeof(resp), &resp_len) &&
            resp_len >= 8 && capnp_read_flat(resp)[0] != 0) {
            return; /* acquired */
        }
        arch_cpu_wait_for_interrupt();
    }
}

static void hw_mutex_unlock(void)
{
    struct core_state *cs = core_state_get();
    if (cs == NULL) {
        return; /* hw_mutex_lock already panicked before this could matter. */
    }
    uint8_t req[24];
    size_t req_len = capnp_build_flat(req, sizeof(req), &cs->hw_mutex_id, 1);
    g_hw_mutex_invoke(cs->hw_mutex_manager, 2, req, req_len, NULL, 0, NULL);
}

/* ---- Jobs: the actual boot-sequence work, run one slice at a time by
 * the scheduler rather than inline in coreinit_main. Every job here
 * finishes in a single slice (returns JOB_FINISHED immediately) -- none
 * of this demo work has a natural "do a little, yield, continue" shape,
 * so there's nothing to gain from spreading it across slices, but the
 * API supports it for jobs that would benefit (see scheduler.h). ---- */

typedef struct {
    const topology_t *topo;
    const struct init_boot_info *info;
    capability_t memory_manager;
    capability_t mutex_manager;
    uint64_t hw_mutex_id;
    capability_t cluster_scheduler;
    capability_t serial_resource;
    int is_bsp;
} smp_job_ctx_t;

typedef struct {
    uint64_t timer_hint; /* arch-opaque timer init hint -- x86_64: the
                          * LAPIC MMIO base; aarch64: the generic-timer
                          * frequency. See topology_timer_hint. */
    int is_bsp;
    uint64_t discovered_calibration; /* only meaningful if !is_bsp --
                                      * see timer.h for why only the BSP
                                      * can measure this itself */
} timer_job_ctx_t;

/* Set by job_timer_bringup on the BSP path, read by job_smp_bringup
 * (which always runs after it -- priority 90 < 100, see coreinit_main)
 * so the value can be published to every AP through the discovery
 * record without needing its own separate plumbing. */
static uint32_t g_timer_calibration = 0;

/* cross_core_signal_test_seen (struct core_state) is set by
 * cross_core_signal_test_handler (an ISR-context callback -- kept to a
 * single flag write, no kprintf from inside the handler itself, matching
 * the same discipline the LAPIC timer ISR already follows) and read back
 * afterward from ordinary code. core_state_get() is a plain register
 * read, safe to call from interrupt context same as anywhere else; a
 * NULL core_state here just means the self-test reports FAIL (the flag
 * was never set), not a hazard the way a NULL hw_mutex would be. */
static void cross_core_signal_test_handler(void)
{
    struct core_state *cs = core_state_get();
    if (cs != NULL) {
        cs->cross_core_signal_test_seen = 1;
    }
}

static job_result_t job_timer_bringup(void *ctx, uint64_t self_job_id,
                                      uint64_t *out_block_id)
{
    (void)out_block_id;
    timer_job_ctx_t *c = (timer_job_ctx_t *)ctx;

    job_kprintf(self_job_id, "\n[coreinit] === job: timer bring-up ===\n");

    if (c->is_bsp) {
        /* Only the BSP may touch the legacy PIC/PIT at all -- IRQ0 is
         * physically wired to the BSP alone on standard PC hardware, so
         * an AP's own copy of the tick counter this measurement needs
         * would never increment. The hardware mutex protects exactly
         * this brief, one-time, shared-hardware-touching window; once
         * calibration is in hand, everything else here is purely
         * per-core LAPIC state needing no cross-core coordination at
         * all. */
        hw_mutex_lock();
        irq_core_init(g_kernel_invoke);
        /* timer_core_init calibrates (x86) or initializes (aarch64) and
         * returns the calibration value for the SMP job to publish. */
        g_timer_calibration =
            (uint32_t)timer_core_init(c->timer_hint, 0, g_kernel_invoke);
        hw_mutex_unlock();
    } else {
        /* irq_core_init() itself still touches genuinely shared hardware
         * on every core that calls it (x86_64's pic_remap/pit_init/
         * IRQ0+1 unmask; aarch64's gic_distributor_init reprograms the
         * one shared GICD wholesale) -- "skipping the legacy PIC/PIT" is
         * true of timer_core_init below (this branch calls
         * lapic_timer_start, never lapic_timer_calibrate, so it touches
         * only this core's own LAPIC/generic timer, same reasoning
         * lapic_timer_x86_64.h's own comment already establishes), but
         * NOT of irq_core_init, which was calling into that shared
         * hardware completely unguarded here -- a real gap against
         * hw_mutex_lock's own stated purpose ("every core's own
         * irq_core_init() would otherwise reconfigure independently
         * with zero arbitration"), caught by re-examining this file
         * rather than introduced by it. Closed the same way the BSP
         * branch above already does, using the AP-to-BSP proxy path
         * coreinit_main already set up for exactly this
         * (cs->hw_mutex_manager), not a new mechanism. */
        hw_mutex_lock();
        irq_core_init(g_kernel_invoke);
        hw_mutex_unlock();
        job_kprintf(self_job_id,
                   "[coreinit]   not the BSP -- using the BSP's own "
                   "published calibration instead of touching the "
                   "legacy PIT ourselves\n");
        timer_core_init(c->timer_hint,
                        (uint32_t)c->discovered_calibration,
                        g_kernel_invoke);
    }

    /* The IDT is now genuinely loaded and stable on this core (both
     * branches above are done clearing/installing gates) -- safe to
     * add our own vector on top without it being wiped by a later
     * irq_core_init call. See cross_core_signal.h: this is the receive
     * side every node needs before anyone else in the cluster can reach
     * it for a preemption signal. */
    cross_core_signal_install_handler(cross_core_signal_test_handler);

    job_kprintf(self_job_id,
               "[coreinit]   cross_core_signal: handler installed, target "
               "%u, self-test...\n",
               (unsigned)cross_core_signal_my_target());
    struct core_state *my_cs = core_state_get();
    if (my_cs != NULL) {
        my_cs->cross_core_signal_test_seen = 0;
    }
    cross_core_signal_send(cross_core_signal_my_target());
    /* A self-IPI via the Local APIC is delivered asynchronously, same
     * as any interrupt -- wait for it to actually land rather than
     * guessing at how many iterations that takes (a fixed busy-count
     * here previously left the interrupt to arrive LATER, during
     * unrelated code, which is a real hardware-level bug in its own
     * right -- see smp.c's lapic_wait_icr_idle). Waiting via
     * arch_cpu_wait_for_interrupt actually stops the core until the next
     * interrupt; bounded so a genuinely broken
     * signal path reports FAIL instead of hanging boot forever. */
    for (int spins = 0;
        spins < 1000 && (my_cs == NULL || !my_cs->cross_core_signal_test_seen);
        spins++) {
        arch_cpu_wait_for_interrupt();
    }
    job_kprintf(self_job_id, "[coreinit]   cross_core_signal: self-test %s\n",
               (my_cs != NULL && my_cs->cross_core_signal_test_seen)
                   ? "PASS" : "FAIL");

    capability_t irq_manager =
        create_object(irq_manager_handler, NULL, &g_irq_manager_table,
                      IRQ_SCHEMA_MANAGER);
    job_kprintf(self_job_id,
               "[coreinit] irq_manager object handle: %u (kept for future "
               "IRQ1+ use -- IRQ0 itself is masked now, see lapic_timer)\n",
               (unsigned)irq_manager.handle);

    capability_t tick_counter =
        create_object(tick_counter_handler, NULL, &g_plain_table,
                      PLAIN_SCHEMA_TICK_COUNTER);
    job_kprintf(self_job_id,
               "[coreinit] tick_counter object handle: %u, bound to this "
               "core's own LAPIC timer\n",
               (unsigned)tick_counter.handle);
    timer_bind(tick_counter);

    return JOB_FINISHED;
}

static job_result_t job_smp_bringup(void *ctx, uint64_t self_job_id,
                                    uint64_t *out_block_id)
{
    (void)self_job_id;
    (void)out_block_id;
    smp_job_ctx_t *c = (smp_job_ctx_t *)ctx;

    kprintf("\n[coreinit] === job: bringing up additional cores ===\n");
    kprintf("[coreinit] found %u topology entries:\n",
            (unsigned)c->topo->count);
    unsigned usable = 0;
    for (size_t i = 0; i < c->topo->count; i++) {
        kprintf("[coreinit]   target=%llu %s\n",
                (unsigned long long)c->topo->cores[i].target,
                c->topo->cores[i].enabled ? "enabled" : "disabled (present "
                                                        "for hotplug only)");
        if (c->topo->cores[i].enabled) {
            usable++;
        }
    }
    kprintf("[coreinit] %u usable core(s) total (including this one)\n",
            usable);

    if (c->is_bsp) {
        smp_bring_up_all(c->topo, c->info, c->memory_manager,
                        c->mutex_manager, c->hw_mutex_id, g_timer_calibration,
                        c->cluster_scheduler, c->serial_resource);
    } else {
        kprintf("[coreinit] not the BSP -- further bring-up stays the "
                "BSP's job, not mine\n");
    }

    return JOB_FINISHED;
}

static job_result_t job_pci_scan(void *ctx, uint64_t self_job_id,
                                 uint64_t *out_block_id)
{
    (void)ctx;
    (void)self_job_id;
    (void)out_block_id;
    if (!pci_present()) {
        kprintf("[coreinit] === job: PCI scan ===\n");
        kprintf("[coreinit]   PCI not present on this platform -- skipping\n");
        return JOB_FINISHED;
    }
    kprintf("\n[coreinit] === job: PCI scan ===\n");
    pci_scan(print_pci_device, NULL);
    return JOB_FINISHED;
}

static job_result_t job_disk_demo(void *ctx, uint64_t self_job_id,
                                  uint64_t *out_block_id)
{
    (void)ctx;
    (void)self_job_id;
    (void)out_block_id;
    if (!ata_present()) {
        kprintf("[coreinit] === job: minting the disk object ===\n");
        kprintf("[coreinit]   ATA not present on this platform -- skipping\n");
        return JOB_FINISHED;
    }
    kprintf("\n[coreinit] === job: minting the disk object (real "
            "ATA-backed) ===\n");
    capability_t disk = create_object(disk_handler, NULL, &g_disk_table,
                                      DISK_SCHEMA_DISK);
    kprintf("[coreinit] disk object handle: %u\n", (unsigned)disk.handle);

    uint64_t lba0 = 0;
    uint8_t disk_req[24];
    size_t disk_req_len = capnp_build_flat(disk_req, sizeof(disk_req), &lba0, 1);
    uint8_t disk_resp[528];
    size_t disk_resp_len = 0;
    g_kernel_invoke(disk, 0, disk_req, disk_req_len, disk_resp,
                    sizeof(disk_resp), &disk_resp_len);
    const uint64_t *sector_data = capnp_read_flat(disk_resp);
    kprintf("[coreinit] sector 0 content: '%s'\n", (const char *)sector_data);
    return JOB_FINISHED;
}

/* Delivered via the cluster scheduler's mailbox mechanism, not
 * submitted locally like the jobs above -- proof the cross-core path
 * genuinely works: ctx here is the SENDING core's own node id (APIC ID
 * on x86_64, MPIDR on aarch64), a plain integer needing no cross-core
 * pointer validity at all. */
static job_result_t job_greet_from_bsp(void *ctx, uint64_t self_job_id,
                                       uint64_t *out_block_id)
{
    (void)self_job_id;
    (void)out_block_id;
    uint64_t sender_apic_id = (uint64_t)(uintptr_t)ctx;
    kprintf("\n[coreinit] === job: greeting received via the cluster "
            "scheduler's cross-node dispatch, sent by node id %u -- "
            "cross-node job delivery works ===\n",
            (unsigned)sender_apic_id);
    return JOB_FINISHED;
}

/* Demonstrates resource declaration + reassignment concretely: declares
 * a stand-in "keyboard" peripheral resource (owned by whoever runs
 * this -- the BSP, since it's only ever submitted there) and attempts
 * to reassign it to node 1 (the first AP in this project's own test
 * setups, if one exists). Submitted at low priority specifically so it
 * runs LAST, after everything else -- by the time a single-box AP's own
 * boot has had this much time, it should already have joined. This is
 * a reasonable assumption for demonstrating the mechanism, not a
 * general "wait for a node" primitive, which this project doesn't have
 * (that would need real synchronization -- a job blocked on "node N has
 * joined" the same way one can already block on a mutex or another
 * job, tracked as a real gap rather than quietly worked around here). */
static job_result_t job_resource_demo(void *ctx, uint64_t self_job_id,
                                      uint64_t *out_block_id)
{
    (void)ctx;
    (void)self_job_id;
    (void)out_block_id;
    kprintf("\n[coreinit] === job: resource declaration + reassignment "
            "demo ===\n");
    uint64_t kb_resource =
        scheduler_declare_resource(RESOURCE_KIND_PERIPHERAL, 0, 0,
                                   RESOURCE_SYNC_TRANSFER);
    kprintf("[coreinit] declared 'keyboard' peripheral resource, id %u, "
            "owned by this node\n",
            (unsigned)kb_resource);
    if (scheduler_reassign_resource(kb_resource, 1)) {
        kprintf("[coreinit] reassigned keyboard resource %u to node 1\n",
                (unsigned)kb_resource);
    } else {
        kprintf("[coreinit] reassignment to node 1 failed -- expected on "
                "a single-core boot, or if node 1 hasn't joined yet\n");
    }
    return JOB_FINISHED;
}

/* Page-fault-driven demand mapping, demonstrated end to end: streaming
 * (not-present -> fault -> transparent resolve -> free thereafter) and
 * copy-on-write (present-but-read-only -> write faults -> private copy
 * -> free thereafter). This is the actual mechanism real swap uses,
 * proven here rather than asserted -- see pagefault.h for the full
 * reasoning. The BSP-only standalone demo uses its own reserved region;
 * the STREAM/COW resource jobs below separately test an AP splitting
 * its OWN page table while the BSP retains the original shared mapping. */
typedef struct {
    uint64_t pml4_phys;
} pagefault_demo_ctx_t;

/* One unclaimed 2 MiB RAM region per architecture. x86 uses 8 MiB,
 * above its low reserved pages and below AP regions; ARM QEMU virt
 * starts RAM at 0x40000000, so its demo cannot use the x86 address. */
#if defined(__aarch64__)
/* QEMU virt's 0x00800000 is GIC MMIO, not RAM. This 2 MiB region sits
 * above all 64 possible AP regions and below the allocation pool. */
#define PF_DEMO_REGION 0x53000000ull
#else
#define PF_DEMO_REGION 0x800000ull
#endif
#define PF_STREAM_PAGE (PF_DEMO_REGION + 0x1000ull)
#define PF_COW_PAGE (PF_DEMO_REGION + 0x2000ull)

static job_result_t job_pagefault_demo(void *ctx, uint64_t self_job_id,
                                       uint64_t *out_block_id)
{
    (void)self_job_id;
    (void)out_block_id;
    pagefault_demo_ctx_t *c = (pagefault_demo_ctx_t *)ctx;

    if (!pagefault_supported()) {
        kprintf("[coreinit]   page-fault demo skipping: this platform has "
                "no page-fault mechanics (see arch_mmu.h)\n");
        return JOB_FINISHED;
    }

    kprintf("[coreinit] pagefault/COW demo starting\n");
    pagefault_init(c->pml4_phys);

    if (!pagefault_split_region(c->pml4_phys, PF_DEMO_REGION)) {
        return JOB_FINISHED;
    }

    volatile uint64_t *stream_ptr = (volatile uint64_t *)PF_STREAM_PAGE;
    *stream_ptr = 0x1111;

    pagefault_mark_absent(c->pml4_phys, PF_STREAM_PAGE);
    uint64_t val = *stream_ptr; /* expect a fault, transparent resolve */
    uint64_t val2 = *stream_ptr; /* expect no new fault */
    kprintf("[coreinit]   stream: read 0x%X/0x%X, faults %u (expect 1, "
           "steady after)\n",
           (unsigned)val, (unsigned)val2, (unsigned)pagefault_get_count());

    volatile uint64_t *cow_ptr = (volatile uint64_t *)PF_COW_PAGE;
    *cow_ptr = 0x2222;
    pagefault_mark_cow(c->pml4_phys, PF_COW_PAGE);
    uint64_t cow_read = *cow_ptr; /* expect no fault -- reads are free */
    uint64_t faults_before_write = pagefault_get_count();
    *cow_ptr = 0x3333; /* expect a fault, private copy made */
    kprintf("[coreinit]   cow: read 0x%X (faults %u), after write faults "
           "%u copies %u, private copy 0x%X (expect 0x3333)\n",
           (unsigned)cow_read, (unsigned)faults_before_write,
           (unsigned)pagefault_get_count(),
           (unsigned)pagefault_get_cow_copy_count(), (unsigned)*cow_ptr);

    return JOB_FINISHED;
}

/* The actual, previously-missing cross-node proof: STREAM and COW
 * resources genuinely accessed by a node that doesn't own them, using
 * that node's own independent page tables (see kernel.c's
 * build_ap_page_tables) -- not just the single-core mechanism
 * demonstration job_pagefault_demo already proved. Two jobs: this one
 * (BSP) allocates a STREAM and a COW resource through the real
 * allocator, writes a known value into each, and hands both resource
 * ids to a job explicitly targeted at node 1; job_stream_cow_verify
 * (below), running ON node 1, is the one that actually calls
 * job_memory_acquire_resource and touches the memory. */
typedef struct {
    uint64_t stream_resource_id;
    uint64_t cow_resource_id;
} stream_demo_ctx_t;

#define STREAM_DEMO_VALUE 0xCAFEBABEull
#define COW_DEMO_VALUE 0xF00DFACEull
#define COW_DEMO_NEW_VALUE 0x5A5A5A5Aull

static job_result_t job_stream_setup(void *ctx, uint64_t self_job_id,
                                     uint64_t *out_block_id)
{
    (void)ctx;
    if (!pagefault_supported()) {
        kprintf("[coreinit]   STREAM/COW setup skipping: this platform has "
                "no page-fault mechanics (see arch_mmu.h)\n");
        return JOB_FINISHED;
    }
    kprintf("\n[coreinit] === job: cross-node STREAM/COW setup (BSP) "
            "===\n");

    if (!scheduler_mutex_try_lock(ALLOCATOR_MUTEX_ID, self_job_id)) {
        *out_block_id = ALLOCATOR_MUTEX_ID;
        return JOB_BLOCKED_ON_MUTEX;
    }
    uint64_t stream_resource_id = 0, stream_base = 0;
    uint64_t cow_resource_id = 0, cow_base = 0;
    scheduler_allocate_slice(4096, RESOURCE_KIND_MEMORY, RESOURCE_SYNC_STREAM,
                             self_job_id, &stream_resource_id, &stream_base);
    scheduler_allocate_slice(4096, RESOURCE_KIND_MEMORY, RESOURCE_SYNC_COW,
                             self_job_id, &cow_resource_id, &cow_base);
    scheduler_mutex_unlock(ALLOCATOR_MUTEX_ID, self_job_id);

    if (stream_resource_id == 0 || cow_resource_id == 0) {
        kprintf("[coreinit] allocation failed -- stream_resource_id=0x%X "
                "cow_resource_id=0x%X\n",
                (unsigned)stream_resource_id, (unsigned)cow_resource_id);
        return JOB_FINISHED;
    }

    *(volatile uint64_t *)stream_base = STREAM_DEMO_VALUE;
    *(volatile uint64_t *)cow_base = COW_DEMO_VALUE;
    kprintf("[coreinit] stream resource 0x%X at 0x%X = 0x%X\n",
            (unsigned)stream_resource_id, (unsigned)stream_base,
            (unsigned)STREAM_DEMO_VALUE);
    kprintf("[coreinit] cow resource 0x%X at 0x%X = 0x%X\n",
            (unsigned)cow_resource_id, (unsigned)cow_base,
            (unsigned)COW_DEMO_VALUE);

    static stream_demo_ctx_t verify_ctx;
    verify_ctx.stream_resource_id = stream_resource_id;
    verify_ctx.cow_resource_id = cow_resource_id;
    uint64_t verify_job = scheduler_submit_to_node(
        1, JOB_TYPE_STREAM_COW_VERIFY, (uint64_t)(uintptr_t)&verify_ctx, 5, 0,
        RESOURCE_KIND_NONE, 0);
    kprintf("[coreinit] submitted verification job 0x%X to node 1\n",
            (unsigned)verify_job);

    return JOB_FINISHED;
}

static job_result_t job_stream_cow_verify(void *ctx, uint64_t self_job_id,
                                          uint64_t *out_block_id)
{
    (void)self_job_id;
    (void)out_block_id;
    stream_demo_ctx_t *c = (stream_demo_ctx_t *)ctx;

    if (!pagefault_supported()) {
        kprintf("[coreinit]   STREAM/COW verify skipping: this platform has "
                "no page-fault mechanics (see arch_mmu.h)\n");
        return JOB_FINISHED;
    }

    kprintf("\n[coreinit] === job: cross-node STREAM/COW verify (node "
            "1) ===\n");

    job_memory_slice_t stream_slice;
    int stream_ok =
        job_memory_acquire_resource(c->stream_resource_id, &stream_slice);
    kprintf("[coreinit] stream acquire: ok=%d owned=%d base=0x%X "
            "sync_kind=%u\n",
            stream_ok, stream_slice.owned, (unsigned)stream_slice.base,
            (unsigned)stream_slice.sync_kind);
    if (stream_ok) {
        uint64_t faults_before = pagefault_get_count();
        uint64_t val = *(volatile uint64_t *)stream_slice.base;
        kprintf("[coreinit] stream read: 0x%X (expect 0x%X), faults "
                "before=%u after=%u\n",
                (unsigned)val, (unsigned)STREAM_DEMO_VALUE,
                (unsigned)faults_before, (unsigned)pagefault_get_count());
        kprintf("[coreinit] stream read matches BSP-written value: %s\n",
                (val == STREAM_DEMO_VALUE) ? "YES" : "NO (BUG)");
    }

    job_memory_slice_t cow_slice;
    int cow_ok = job_memory_acquire_resource(c->cow_resource_id, &cow_slice);
    kprintf("[coreinit] cow acquire: ok=%d owned=%d base=0x%X "
            "sync_kind=%u\n",
            cow_ok, cow_slice.owned, (unsigned)cow_slice.base,
            (unsigned)cow_slice.sync_kind);
    if (cow_ok) {
        uint64_t faults_before_read = pagefault_get_count();
        uint64_t val = *(volatile uint64_t *)cow_slice.base;
        kprintf("[coreinit] cow read: 0x%X (expect 0x%X), faults "
                "before=%u after=%u (should be unchanged -- reads are "
                "free)\n",
                (unsigned)val, (unsigned)COW_DEMO_VALUE,
                (unsigned)faults_before_read, (unsigned)pagefault_get_count());
        kprintf("[coreinit] cow read matches BSP-written value: %s\n",
                (val == COW_DEMO_VALUE) ? "YES" : "NO (BUG)");

        uint64_t copies_before = pagefault_get_cow_copy_count();
        *(volatile uint64_t *)cow_slice.base = COW_DEMO_NEW_VALUE;
        uint64_t readback = *(volatile uint64_t *)cow_slice.base;
        kprintf("[coreinit] cow write triggered a copy: %s (copies %u -> "
                "%u)\n",
                (pagefault_get_cow_copy_count() > copies_before) ? "YES"
                                                                 : "NO (BUG)",
                (unsigned)copies_before,
                (unsigned)pagefault_get_cow_copy_count());
        kprintf("[coreinit] cow private copy readback: 0x%X (expect "
                "0x%X): %s\n",
                (unsigned)readback, (unsigned)COW_DEMO_NEW_VALUE,
                (readback == COW_DEMO_NEW_VALUE) ? "YES" : "NO (BUG)");
    }

    return JOB_FINISHED;
}

/* Stage-2 reports through an ordinary object, never a direct pointer to
 * coreinit's serial formatter. Its own status object forwards this one
 * boot marker through kernel_invoke, demonstrating the object-only seam. */
static void securinit_status_sink(void *data, uint32_t method_id,
                                 const void *msg, size_t len, void *response,
                                 size_t response_cap, size_t *out_response_len)
{
    (void)data;
    (void)method_id;
    (void)len;
    (void)response;
    (void)response_cap;
    *out_response_len = 0;
    uint64_t marker = capnp_read_flat(msg)[0];
    if (marker == SECURINIT_BOOT_MARKER) {
        kprintf("[securinit] stage 2 entered through the standard IPC object interface\n");
    } else if (marker == SECURINIT_EL0_DENIED_MARKER) {
        kprintf("[securinit] EL0 hardware denied access to Admin; wrapper regained EL1\n");
    } else if (marker == SECURINIT_EL0_IPC_MARKER) {
        kprintf("[securinit] EL0 outbound Cap'n Proto IPC traversed wrapper-owned Gate\n");
    } else if (marker == SECURINIT_RING3_DENIED_MARKER) {
        kprintf("[securinit] ring3 hardware denied access to Admin; wrapper regained ring0\n");
    } else if (marker == SECURINIT_RING3_IPC_MARKER) {
        kprintf("[securinit] ring3 outbound Cap'n Proto IPC traversed wrapper-owned Gate\n");
    }
}

#if defined(__aarch64__)
static void securinit_exception_router(void *data, uint32_t method_id,
                                      const void *msg, size_t len, void *response,
                                      size_t response_cap, size_t *out_response_len)
{
    (void)data;
    (void)method_id;
    (void)len;
    uint64_t handler = capnp_read_flat(msg)[0];
    uint64_t ok = handler != 0 && arch_mmu_root_phys() != 0;
    if (ok) {
        pagefault_aarch64_set_el0_hook(
            (pagefault_el0_hook_t)(uintptr_t)handler);
    }
    *out_response_len = capnp_build_flat(response, response_cap, &ok, 1);
}
#elif defined(__x86_64__)
static void securinit_ring3_router(void *data, uint32_t method_id,
                                   const void *msg, size_t len, void *response,
                                   size_t response_cap, size_t *out_response_len)
{
    (void)data;
    (void)method_id;
    (void)len;
    const uint64_t *words = capnp_read_flat(msg);
    uint64_t ok = irq_x86_64_install_wrapper((void *)(uintptr_t)words[0],
                                               (void *)(uintptr_t)words[1],
                                               words[2]);
    if (ok && words[3]) {
        pagefault_x86_64_set_ring3_hook(
            (pagefault_ring3_hook_t)(uintptr_t)words[3]);
    }
    *out_response_len = capnp_build_flat(response, response_cap, &ok, 1);
}
#endif

static job_result_t job_securinit_boot(void *ctx, uint64_t self_job_id,
                                      uint64_t *out_block_id)
{
    (void)ctx;
    if (!scheduler_mutex_try_lock(ALLOCATOR_MUTEX_ID, self_job_id)) {
        *out_block_id = ALLOCATOR_MUTEX_ID;
        return JOB_BLOCKED_ON_MUTEX;
    }
    uint64_t resource_id = 0, base = 0;
    int allocated = scheduler_allocate_slice(0x200000, RESOURCE_KIND_MEMORY,
                                              RESOURCE_SYNC_TRANSFER, self_job_id,
                                              &resource_id, &base);
    /* The wrapper owns a separate 2 MiB-aligned user region, never the
     * same page-table block as its own privileged Mach-O. */
    const uint64_t user_region = ALLOC_MIN + 0x800000ull;
    uint64_t user_resource_id = 0;
    int user_allocated = allocated && scheduler_request_allocation(
        user_region, 0x200000, RESOURCE_KIND_MEMORY, RESOURCE_SYNC_TRANSFER,
        &user_resource_id);
    scheduler_mutex_unlock(ALLOCATOR_MUTEX_ID, self_job_id);
    if (!allocated || !resource_id) {
        kprintf("[coreinit] securinit: could not claim an image region\n");
        return JOB_FINISHED;
    }
    if (!user_allocated || !user_resource_id) {
        kprintf("[coreinit] securinit: could not claim isolated user region\n");
        return JOB_FINISHED;
    }

    size_t length = (size_t)(securinit_image_end - securinit_image_start);
    uint64_t entry = stage_load_macho(securinit_image_start, length, base,
                                       0x200000);
    if (!entry) {
        kprintf("[coreinit] securinit: invalid stage-2 Mach-O\n");
        return JOB_FINISHED;
    }
    capability_t sink = create_object(securinit_status_sink, NULL,
                                      &g_plain_table, PLAIN_SCHEMA_TICK_COUNTER);
    if (!sink.handle) {
        return JOB_FINISHED;
    }
    struct securinit_boot_info handoff = {
        .kernel_invoke = g_kernel_invoke,
        .factory = g_factory,
        .status_sink = sink,
        .user_region_base = user_region,
        .user_region_size = 0x200000,
    };
#if defined(__aarch64__)
    capability_t router = create_object(securinit_exception_router, NULL,
                                        &g_el0_exception_router_table, 0);
    if (!router.handle) {
        return JOB_FINISHED;
    }
    handoff.exception_router = router;
    handoff.exception_router_bind_method = EL0_EXCEPTION_ROUTER_METHOD_BIND;
    handoff.ttbr0_root_phys = arch_mmu_root_phys();
#elif defined(__x86_64__)
    capability_t router = create_object(securinit_ring3_router, NULL,
                                        &g_ring3_exception_router_table, 0);
    if (!router.handle) {
        return JOB_FINISHED;
    }
    handoff.exception_router = router;
    handoff.exception_router_bind_method = RING3_EXCEPTION_ROUTER_METHOD_BIND;
    handoff.x86_pml4_phys = g_boot_pml4_phys;
#endif
    uint64_t (*start)(const struct securinit_boot_info *) =
        (uint64_t (*)(const struct securinit_boot_info *))(uintptr_t)entry;
    uint64_t gate_handle = start(&handoff);
    if (!gate_handle) {
        kprintf("[coreinit] securinit: stage-2 object bootstrap failed\n");
        return JOB_FINISHED;
    }
    kprintf("[coreinit] securinit image at 0x%llX, Gate object 0x%llX\n",
            (unsigned long long)base, (unsigned long long)gate_handle);
    uint64_t unused = 0;
    uint8_t stats_req[24], stats_resp[40];
    size_t req_len = capnp_build_flat(stats_req, sizeof(stats_req), &unused, 1);
    size_t resp_len = 0;
    capability_t gate = {gate_handle};
    if (g_kernel_invoke(gate, GATE_METHOD_GET_STATS, stats_req, req_len,
                        stats_resp, sizeof(stats_resp), &resp_len) &&
        resp_len >= 32) {
        const uint64_t *stats = capnp_read_flat(stats_resp);
        kprintf("[securinit] Gate denied %llu request(s), forwarded %llu\n",
                (unsigned long long)stats[0], (unsigned long long)stats[1]);
    }
    return JOB_FINISHED;
}

/* Registers the identical set of (type, fn) pairs every core's
 * bootstrap uses -- see scheduler.h's own note on why this must be the
 * SAME set everywhere: a job_type value crossing a core boundary is
 * only meaningful if every core resolves it to the matching function. */
static void register_job_types(void)
{
    scheduler_register_job_type(JOB_TYPE_PCI_SCAN, job_pci_scan);
    scheduler_register_job_type(JOB_TYPE_DISK_DEMO, job_disk_demo);
    scheduler_register_job_type(JOB_TYPE_TIMER_BRINGUP, job_timer_bringup);
    scheduler_register_job_type(JOB_TYPE_SMP_BRINGUP, job_smp_bringup);
    scheduler_register_job_type(JOB_TYPE_GREET_FROM_BSP, job_greet_from_bsp);
    scheduler_register_job_type(JOB_TYPE_RESOURCE_DEMO, job_resource_demo);
    scheduler_register_job_type(JOB_TYPE_PAGEFAULT_DEMO, job_pagefault_demo);
    scheduler_register_job_type(JOB_TYPE_STREAM_SETUP, job_stream_setup);
    scheduler_register_job_type(JOB_TYPE_STREAM_COW_VERIFY,
                                job_stream_cow_verify);
    scheduler_register_job_type(JOB_TYPE_SECURINIT_BOOT, job_securinit_boot);
}

void coreinit_main(struct init_boot_info *info)
{
    serial_init();

    /* BSP vs AP: info->bsp_kernel_invoke_addr is 0 only for the BSP's own
     * boot_info (see that field's own comment in init_abi.h for why this
     * doubles as the canonical role signal rather than a separate field).
     * Computed once, up here, so the core_state scaffolding right below
     * and the shared-services setup further down both read the same
     * value instead of each deriving their own. */
    int is_bsp = (info->bsp_kernel_invoke_addr == 0);

    /* Scaffolding for the shared-code/per-core-struct redesign (see
     * TODO.md): this core's own core_state pointer, set once here so
     * later migrations have somewhere to reach it from a register
     * instead of a global. Deliberately no behavior change yet -- every
     * subsystem below still reads its own existing global, and nothing
     * here skips any step based on is_bsp. The BSP's own instance is
     * static/image-resident (one, known size, no allocation needed); an
     * AP's is bump-allocated from the free-memory pool info carries (see
     * core_state.h) and may come back NULL if that pool wasn't supplied
     * -- non-fatal, since nothing yet depends on it. */
    if (is_bsp) {
        static struct core_state bsp_core_state;
        core_state_init_defaults(&bsp_core_state);
        core_state_set(&bsp_core_state);
    } else {
        struct core_state *cs = core_state_alloc_ap(info);
        if (cs != NULL) {
            core_state_init_defaults(cs);
            core_state_set(cs);
        } else {
            kprintf("[coreinit] no free-memory pool for this core's "
                    "core_state -- continuing without one\n");
        }
    }

    /* The Mach-O loader may place this instance at an AP-specific address.
     * The generated arrays contain only const indices, while these static
     * descriptor bridges must point to THIS copy's arrays. No heap or
     * schema parsing is involved. */
    coreinit_schemas_init();
    static topology_t topo;
    int have_topo = 0;
#if defined(__aarch64__)
    /* This core owns its identity map; the bootloader and PSCI supply none.
     * Keep the same physical addresses before and after enabling it, then
     * discover the UART from FDT or ACPI SPCR before the stage-1 print. */
    int mmu_ready = arch_mmu_enable_identity_map();
    have_topo = topology_discover(&topo, topology_boot_hint(info));
    kprintf("[coreinit] aarch64 EL1 identity MMU: %s (root 0x%llX)\n",
            mmu_ready ? "enabled" : "unavailable",
            (unsigned long long)arch_mmu_root_phys());
#endif
    g_kernel_invoke = info->kernel_invoke;
#if defined(__x86_64__)
    g_boot_pml4_phys = info->pml4_phys;
#endif
    g_factory = info->caps.factory;
    proxy_init(g_kernel_invoke, g_factory);

    /* Read this core's own ID directly from hardware (LAPIC ID register
     * on x86_64, MPIDR_EL1 on aarch64) as the very first thing, so EVERY
     * line of output from here on is correctly tagged -- without this,
     * the shared-services setup below (which happens before topology
     * discovery would otherwise give us this) prints untagged, and on a
     * shared, unlocked UART that reads as attributed to whichever OTHER
     * core happened to interleave a tagged line at the same moment.
     * This value is also the node id used for the cluster-scheduler
     * join below: the architecture's own core identity -- APIC ID on
     * x86_64, MPIDR affinity on aarch64 -- which is what
     * smp_aarch64.c's greeting-job affinity matches against. */
    uint64_t my_node_id = topology_self_id();
    serial_set_core_tag((int)my_node_id);

    kprintf("[coreinit] stage 1: running on the single-syscall "
            "kernel_invoke model\n");
    kprintf("[coreinit] received %u initial capabilities from the kernel\n",
            (unsigned)info->caps.count);

    /* Shared services: created here if this is the BSP; discovered via
     * the record smp.c wrote into this exact region if this is an AP.
     * Needed before job_timer_bringup runs (it guards PIC/PIT access
     * with the hardware mutex) and before job_smp_bringup runs (it
     * claims regions via the memory manager) -- both jobs, but this
     * part stays plain bootstrap since everything after it depends on
     * it existing. is_bsp itself was already computed above, before the
     * core_state scaffolding. */

    /* This core's own pml4_phys -- see kernel.c's build_ap_page_tables
     * for why every core now has an independent one -- needed by
     * job_memory.c for any STREAM/COW slice's local fault setup. */
    job_memory_init(info->pml4_phys);
    capability_t memory_manager = {0};
    capability_t mutex_manager = {0};

    if (is_bsp) {
        kprintf("\n[coreinit] === creating shared services (mutex "
                "manager, memory manager) before any core is brought up "
                "===\n");
        mutex_manager_init_schema();
        mutex_manager = create_object(mutex_manager_handler, NULL,
                                      &g_mutex_manager_table,
                                      MUTEX_MGR_SCHEMA_MUTEX_MANAGER);
        memory_manager_init_schema();
        memory_manager = create_object(memory_manager_handler, NULL,
                                       &g_memory_manager_table,
                                       MEM_MGR_SCHEMA_MEMORY_MANAGER);
        kprintf("[coreinit] mutex_manager handle %u, memory_manager "
                "handle %u\n",
                (unsigned)mutex_manager.handle,
                (unsigned)memory_manager.handle);

        uint64_t dummy = 0;
        uint8_t create_req[24];
        size_t create_req_len =
            capnp_build_flat(create_req, sizeof(create_req), &dummy, 1);
        uint8_t create_resp[24];
        size_t create_resp_len = 0;
        g_kernel_invoke(mutex_manager, 0, create_req, create_req_len,
                        create_resp, sizeof(create_resp), &create_resp_len);
        const uint64_t *id_out = capnp_read_flat(create_resp);

        g_hw_mutex_invoke = g_kernel_invoke;
        struct core_state *cs = core_state_get();
        if (cs != NULL) {
            cs->hw_mutex_manager = mutex_manager;
            cs->hw_mutex_id = id_out[0];
        }
        kprintf("[coreinit] hardware mutex created, id %u\n",
                (unsigned)id_out[0]);
    } else {
        kprintf("\n[coreinit] === discovered shared services from the BSP "
                "===\n");
        /* The PROXY objects we're about to create are validated by the
         * LOCAL kernel against the schema THEY were created with, not
         * against whatever the real remote object implements -- so this
         * node references the same const table and index (every node has
         * one, being byte-identical compiled code) even though it will
         * never run the real handler logic locally. With index-based
         * schemas there is nothing to populate first: the table is const
         * data, identical on every instance, which is what let the old
         * per-instance *_build_schema calls disappear. */
        remote_invoke_fn_t bsp_invoke =
            (remote_invoke_fn_t)(uintptr_t)info->bsp_kernel_invoke_addr;
        capability_t remote_mutex_manager;
        remote_mutex_manager.handle = info->mutex_manager_handle;

        /* A LOCAL proxy, not the raw remote pair -- from here on, this
         * core's own core_state->hw_mutex_manager is an ordinary local
         * capability, invoked through the ordinary local
         * g_kernel_invoke, same as anything else. hw_mutex_lock/unlock
         * don't need to know or care that it's actually remote. */
        capability_t local_hw_mutex_manager =
            create_proxy(remote_mutex_manager, bsp_invoke,
                        &g_mutex_manager_table,
                        MUTEX_MGR_SCHEMA_MUTEX_MANAGER);
        g_hw_mutex_invoke = g_kernel_invoke;
        struct core_state *cs = core_state_get();
        if (cs != NULL) {
            cs->hw_mutex_manager = local_hw_mutex_manager;
            cs->hw_mutex_id = info->hardware_mutex_id;
        }
        kprintf("[coreinit] mutex_manager reachable via local proxy handle "
                "%u, hardware mutex id %u\n",
                (unsigned)local_hw_mutex_manager.handle,
                (unsigned)info->hardware_mutex_id);
    }

    /* The main cluster scheduler: created here (BSP) or discovered here
     * (AP), same pattern as mutex_manager/memory_manager above --
     * except this one ALSO needs registering with, since it's the thing
     * that's supposed to know every core exists in the first place. */
    capability_t cluster_scheduler = {0};
    if (is_bsp) {
        cluster_scheduler_init_schema(memory_manager, g_kernel_invoke,
                                      g_factory);
        cluster_scheduler = create_object(cluster_scheduler_handler, NULL,
                                          &g_cluster_scheduler_table,
                                          CS_SCHEMA_CLUSTER_SCHEDULER);
        kprintf("[coreinit] cluster_scheduler handle %u\n",
                (unsigned)cluster_scheduler.handle);
    } else {
        capability_t remote_cluster_scheduler;
        remote_cluster_scheduler.handle = info->cluster_scheduler_handle;
        remote_invoke_fn_t bsp_invoke =
            (remote_invoke_fn_t)(uintptr_t)info->bsp_kernel_invoke_addr;
        cluster_scheduler = create_proxy(remote_cluster_scheduler, bsp_invoke,
                                         &g_cluster_scheduler_table,
                                         CS_SCHEMA_CLUSTER_SCHEDULER);
        kprintf("[coreinit] cluster_scheduler reachable via local proxy "
                "handle %u\n",
                (unsigned)cluster_scheduler.handle);
    }

    kprintf("\n[coreinit] === real validation: malformed request rejected "
            "before dispatch ===\n");
    uint8_t too_short[4] = {0, 0, 0, 0};
    uint8_t resp[8];
    size_t resp_len = 0;
    int ok = g_kernel_invoke(g_factory, 0, too_short, sizeof(too_short),
                             resp, sizeof(resp), &resp_len);
    kprintf("[coreinit] malformed create call result: %s (expected: "
            "rejected)\n",
            ok ? "ACCEPTED (bug!)" : "rejected");

    if (is_bsp) {
        /* Before ANY code can possibly enable interrupts (which
         * run_job_exec_test's very first job_exec_switch does, via a
         * fake frame with IF=1) -- the PIC must be remapped first so
         * IRQ0 doesn't collide with vector 8 (double fault). This is
         * done inside irq_core_init (called later, via
         * job_timer_bringup), but we need the IDT loaded NOW. */
        irq_core_init(g_kernel_invoke);
        run_job_exec_test();
        run_job_exec_priority_test();
    }

    /* ARM discovers topology at entry to wire serial before the first
     * print. x86 discovers here, before timer and SMP bring-up. */
#if !defined(__aarch64__)
    have_topo = topology_discover(&topo, topology_boot_hint(info));
#endif
    if (have_topo) {
        kprintf("[coreinit] this core's own ID: %llu\n",
                (unsigned long long)topology_self_id());
    } else {
        kprintf("[coreinit] topology discovery failed -- no timer bring-up or "
                "further cores possible this boot\n");
    }

    /* Cross-node scheduling setup: register the identical job-type
     * vocabulary every node registers, tell the client layer how to
     * reach the cluster scheduler, and join the cluster. All of this is
     * meaningful even before ACPI succeeds (job-type registration
     * doesn't depend on it), so it happens unconditionally; only
     * join_node needs a real node id, which defaults sensibly to 0 if
     * ACPI discovery somehow failed. */
    register_job_types();
    /* Always the LOCAL kernel_invoke now -- cluster_scheduler is either
     * genuinely local (BSP) or a local proxy (AP), so there's no more
     * "which invoke function" special-casing needed here at all. */
    scheduler_client_init(g_kernel_invoke, cluster_scheduler, (uint8_t)my_node_id);

    /* The architecture code this node joins as -- resolved per-arch, like
     * everything else in topology. */
    uint64_t reg_words[3] = {my_node_id, topology_cluster_arch(),
                             cross_core_signal_my_target()};
    uint8_t reg_req[48];
    size_t reg_req_len = capnp_build_flat(reg_req, sizeof(reg_req), reg_words, 3);
    g_kernel_invoke(cluster_scheduler, 0, reg_req, reg_req_len, NULL, 0, NULL);

    /* Join is identity only -- resources are this node's own to
     * declare afterward, the same declare_resource path everything
     * else (the shared serial resource just below, a job's own
     * peripheral demo, ...) already uses, not something join hands out
     * automatically. cpu_core mirrors what every node has always
     * implicitly provided. The MMU resource is advertised only when
     * translation AND fault resolution work on this node. */
    uint64_t my_cpu_core = scheduler_declare_resource(
        RESOURCE_KIND_CPU_CORE, topology_cluster_arch(), 0,
        RESOURCE_SYNC_TRANSFER);
    kprintf("[coreinit] declared this node's own cpu_core resource, id %u\n",
           (unsigned)my_cpu_core);
    /* STREAM/COW users interpret this resource as an actual ability to
     * fault and resolve pages, not merely an enabled translation bit. */
    if (arch_has_mmu() && pagefault_supported()) {
        uint64_t my_mmu = scheduler_declare_resource(
            RESOURCE_KIND_MMU, topology_cluster_arch(), 0,
            RESOURCE_SYNC_TRANSFER);
        kprintf("[coreinit] declared this node's own mmu resource, id %u\n",
               (unsigned)my_mmu);
    }

    /* The shared "serial" resource: declared once by the BSP (after
     * scheduler_client_init, so scheduler_declare_resource has
     * somewhere to send the request), discovered/proxied by an AP the
     * same way it already reaches mutex_manager/cluster_scheduler. Node
     * 0 owns it administratively; sync_kind is irrelevant here since
     * nothing ever transfers or streams it, only reserves it briefly. */
    if (is_bsp) {
        g_serial_resource.handle = scheduler_declare_resource(
            RESOURCE_KIND_PERIPHERAL, 0, 0, RESOURCE_SYNC_TRANSFER);
        kprintf("[coreinit] serial resource declared, handle 0x%X\n",
                (unsigned)g_serial_resource.handle);
    } else {
        capability_t remote_serial;
        remote_serial.handle = info->serial_resource_handle;
        remote_invoke_fn_t bsp_invoke =
            (remote_invoke_fn_t)(uintptr_t)info->bsp_kernel_invoke_addr;
        g_serial_resource =
            create_proxy(remote_serial, bsp_invoke, &g_resource_table,
                         CS_SCHEMA_RESOURCE);
        kprintf("[coreinit] serial resource reachable via local proxy "
                "handle 0x%X\n",
                (unsigned)g_serial_resource.handle);
    }

    kprintf("\n[coreinit] === submitting jobs, then handing off to the "
            "scheduler ===\n");

    if (have_topo) {
        static timer_job_ctx_t timer_ctx;
        /* The hint is arch-opaque (x86_64: LAPIC base; aarch64:
         * generic-timer frequency) -- topology_timer_hint returns the
         * right slot for whichever arch this build is. */
        timer_ctx.timer_hint = topology_timer_hint(&topo);
        timer_ctx.is_bsp = is_bsp;
        timer_ctx.discovered_calibration = info->lapic_calibration_count;
        scheduler_submit(JOB_TYPE_TIMER_BRINGUP, &timer_ctx, 100, 0, RESOURCE_KIND_NONE, 0);

        static smp_job_ctx_t smp_ctx;
        smp_ctx.topo = &topo;
        smp_ctx.info = info;
        smp_ctx.memory_manager = memory_manager;
        smp_ctx.mutex_manager = mutex_manager;
        {
            struct core_state *this_cs = core_state_get();
            smp_ctx.hw_mutex_id = this_cs != NULL ? this_cs->hw_mutex_id : 0;
        }
        smp_ctx.cluster_scheduler = cluster_scheduler;
        smp_ctx.serial_resource = g_serial_resource;
        smp_ctx.is_bsp = is_bsp;
        scheduler_submit(JOB_TYPE_SMP_BRINGUP, &smp_ctx, 90, 0, RESOURCE_KIND_NONE, 0);
    }

    scheduler_submit(JOB_TYPE_PCI_SCAN, NULL, 50, 0, RESOURCE_KIND_NONE, 0);
    scheduler_submit(JOB_TYPE_DISK_DEMO, NULL, 50, 0, RESOURCE_KIND_NONE, 0);
    if (is_bsp) {
        scheduler_submit(JOB_TYPE_RESOURCE_DEMO, NULL, 10, 0,
                         RESOURCE_KIND_NONE, 0);

        static pagefault_demo_ctx_t pf_ctx;
        pf_ctx.pml4_phys = info->pml4_phys;
        scheduler_submit(JOB_TYPE_PAGEFAULT_DEMO, &pf_ctx, 5, 0,
                         RESOURCE_KIND_NONE, 0);
#if defined(__aarch64__)
        /* After PSCI bring-up, prove an AP uses its OWN page tables to
         * stream and COW-copy BSP-owned resource pages. */
        if (have_topo && topo.count > 1 && pagefault_supported()) {
            scheduler_submit(JOB_TYPE_STREAM_SETUP, NULL, 4, 0,
                             RESOURCE_KIND_NONE, 0);
        }
#endif
        scheduler_submit(JOB_TYPE_SECURINIT_BOOT, NULL, 3, 0,
                         RESOURCE_KIND_NONE, 0);
    }

    kprintf("\n[coreinit] securinit image queued after hardware bring-up; "
            "authorization awaits a trusted isolation boundary\n");

    scheduler_run();
}
