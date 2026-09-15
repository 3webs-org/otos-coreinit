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
 * This file is compiled as a Mach-O binary (clang -target
 * x86_64-apple-darwin), not ELF.
 */

#include "arch_x86_64.h"
#include "acpi.h"
#include "ata.h"
#include "cap.h"
#include "capnp_build.h"
#include "cluster_scheduler.h"
#include "arch_mmu.h"
#include "cross_core_signal.h"
#include "irq.h"
#include "lapic_timer.h"
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
#include "../include/init_abi.h"
#include "../include/smp_layout.h"

/* NOTE on mode64_token: see kernel/arch_x86_64.h's own comment for why
 * the kernel's copy of these primitives is gated and coreinit's copy
 * (this file's own arch_x86_64.h) is not. */

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
                                  const object_schema_t *schema)
{
    if (g_bootstrap_record_next >= BOOTSTRAP_RECORD_POOL_SIZE) {
        kprintf("[coreinit] create_object: FAILED, bootstrap record pool "
                "exhausted\n");
        capability_t none = {0};
        return none;
    }
    void *record_storage = g_bootstrap_records[g_bootstrap_record_next++];

    uint64_t words[4] = {(uint64_t)(uintptr_t)handler,
                         (uint64_t)(uintptr_t)data,
                         (uint64_t)(uintptr_t)schema,
                         (uint64_t)(uintptr_t)record_storage};
    uint8_t req[64];
    size_t req_len = capnp_build_flat(req, sizeof(req), words, 4);

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
        __asm__ volatile("hlt");
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
        __asm__ volatile("hlt");
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
        __asm__ volatile("hlt");
    }
}

static void job_exec_priority_test_med(uint64_t arg)
{
    (void)arg;
    record_test_event(EVT_MED_RAN);
    job_exec_wait(&g_test_med_done_q); /* park forever */
    for (;;) {
        __asm__ volatile("hlt");
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
        __asm__ volatile("hlt");
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

static const struct_schema_t g_msg_schema = {
    .data_words = 1, .pointer_count = 0, .pointers = NULL};
/* See kernel/cap.c's own comment on init_object_zero_schema for why
 * these are runtime-populated rather than const-initialized with baked
 * pointers -- same reasoning, same fix, needed here too since coreinit
 * is exactly as subject to being loaded at a discovered-at-runtime base
 * address as the kernel is. */
static method_schema_t g_plain_methods[1];
static object_schema_t g_plain_schema;

static void init_plain_schema(void)
{
    g_plain_methods[0].method_id = 0;
    g_plain_methods[0].param_schema = &g_msg_schema;
    g_plain_methods[0].result_schema = NULL;
    g_plain_schema.methods = g_plain_methods;
    g_plain_schema.method_count = 1;
}

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

static const struct_schema_t g_disk_request_schema = {
    .data_words = 1, .pointer_count = 0, .pointers = NULL}; /* {lba: u64} */
static const struct_schema_t g_disk_response_schema = {
    .data_words = 64, .pointer_count = 0, .pointers = NULL}; /* 512 bytes */
static method_schema_t g_disk_methods[1];
static object_schema_t g_disk_schema;

static void init_disk_schema(void)
{
    g_disk_methods[0].method_id = 0;
    g_disk_methods[0].param_schema = &g_disk_request_schema;
    g_disk_methods[0].result_schema = &g_disk_response_schema;
    g_disk_schema.methods = g_disk_methods;
    g_disk_schema.method_count = 1;
}

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
 * hardware (the PIC, the PIT) that every core's own irq_init() would
 * otherwise reconfigure independently with zero arbitration. Reaching
 * it is either an ordinary local invoke (BSP, which hosts it directly)
 * or a call through the BSP's OWN kernel_invoke at its own address
 * (AP, via the discovery record) -- the lock/unlock call sites below
 * don't need to know or care which. ---- */

static int (*g_hw_mutex_invoke)(capability_t, uint32_t, const void *, size_t,
                                void *, size_t, size_t *);
static capability_t g_hw_mutex_manager;
static uint64_t g_hw_mutex_id;

static void hw_mutex_lock(void)
{
    uint8_t req[24];
    size_t req_len = capnp_build_flat(req, sizeof(req), &g_hw_mutex_id, 1);
    /* METHOD_LOCK is try-only (see mutex_mgr.c) -- it never blocks
     * inside the handler, so contention is handled HERE, by actually
     * yielding the core (hlt, woken by the next interrupt) between
     * attempts rather than spinning. */
    for (;;) {
        uint8_t resp[24];
        size_t resp_len = 0;
        if (g_hw_mutex_invoke(g_hw_mutex_manager, 1, req, req_len, resp,
                              sizeof(resp), &resp_len) &&
            resp_len >= 8 && capnp_read_flat(resp)[0] != 0) {
            return; /* acquired */
        }
        __asm__ volatile("hlt");
    }
}

static void hw_mutex_unlock(void)
{
    uint8_t req[24];
    size_t req_len = capnp_build_flat(req, sizeof(req), &g_hw_mutex_id, 1);
    g_hw_mutex_invoke(g_hw_mutex_manager, 2, req, req_len, NULL, 0, NULL);
}

/* ---- Jobs: the actual boot-sequence work, run one slice at a time by
 * the scheduler rather than inline in coreinit_main. Every job here
 * finishes in a single slice (returns JOB_FINISHED immediately) -- none
 * of this demo work has a natural "do a little, yield, continue" shape,
 * so there's nothing to gain from spreading it across slices, but the
 * API supports it for jobs that would benefit (see scheduler.h). ---- */

typedef struct {
    const acpi_info_t *acpi;
    uint8_t bsp_apic_id;
    const struct init_boot_info *info;
    capability_t memory_manager;
    capability_t mutex_manager;
    uint64_t hw_mutex_id;
    capability_t cluster_scheduler;
    capability_t serial_resource;
    int is_bsp;
} smp_job_ctx_t;

typedef struct {
    const acpi_info_t *acpi;
    int is_bsp;
    uint64_t discovered_lapic_calibration; /* only meaningful if !is_bsp --
                                            * see lapic_timer.h for why
                                            * only the BSP can measure
                                            * this itself */
} timer_job_ctx_t;

/* Set by job_timer_bringup on the BSP path, read by job_smp_bringup
 * (which always runs after it -- priority 90 < 100, see coreinit_main)
 * so the value can be published to every AP through the discovery
 * record without needing its own separate plumbing. */
static uint32_t g_lapic_calibration = 0;

/* Set by cross_core_signal_test_handler (an ISR-context callback --
 * kept to a single flag write, no kprintf from inside the handler
 * itself, matching the same discipline the LAPIC timer ISR already
 * follows) and read back afterward from ordinary code. */
static volatile int g_cross_core_signal_test_seen;

static void cross_core_signal_test_handler(void)
{
    g_cross_core_signal_test_seen = 1;
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
        irq_init(g_kernel_invoke);
        uint32_t count = lapic_timer_calibrate(c->acpi->local_apic_address);
        hw_mutex_unlock();

        lapic_timer_start(c->acpi->local_apic_address, count, g_kernel_invoke);
        g_lapic_calibration = count;
    } else {
        job_kprintf(self_job_id,
                   "[coreinit]   not the BSP -- skipping the legacy PIC/PIT "
                   "entirely, using the BSP's own published calibration "
                   "instead\n");
        irq_setup_idt_only();
        lapic_timer_start(c->acpi->local_apic_address,
                          (uint32_t)c->discovered_lapic_calibration,
                          g_kernel_invoke);
    }

    /* The IDT is now genuinely loaded and stable on this core (both
     * branches above are done clearing/installing gates) -- safe to
     * add our own vector on top without it being wiped by a later
     * irq_init/irq_setup_idt_only call. See cross_core_signal.h: this
     * is the receive side every node needs before anyone else in the
     * cluster can reach it for a preemption signal. */
    cross_core_signal_install_handler(cross_core_signal_test_handler);

    job_kprintf(self_job_id,
               "[coreinit]   cross_core_signal: handler installed, target "
               "%u, self-test...\n",
               (unsigned)cross_core_signal_my_target());
    g_cross_core_signal_test_seen = 0;
    cross_core_signal_send(cross_core_signal_my_target());
    /* A self-IPI via the Local APIC is delivered asynchronously, same
     * as any interrupt -- wait for it to actually land rather than
     * guessing at how many iterations that takes (a fixed busy-count
     * here previously left the interrupt to arrive LATER, during
     * unrelated code, which is a real hardware-level bug in its own
     * right -- see smp.c's lapic_wait_icr_idle). hlt actually stops
     * the core until the next interrupt; bounded so a genuinely broken
     * signal path reports FAIL instead of hanging boot forever. */
    for (int spins = 0; spins < 1000 && !g_cross_core_signal_test_seen;
        spins++) {
        __asm__ volatile("hlt");
    }
    job_kprintf(self_job_id, "[coreinit]   cross_core_signal: self-test %s\n",
               g_cross_core_signal_test_seen ? "PASS" : "FAIL");

    capability_t irq_manager =
        create_object(irq_manager_handler, NULL, &g_irq_manager_schema);
    job_kprintf(self_job_id,
               "[coreinit] irq_manager object handle: %u (kept for future "
               "IRQ1+ use -- IRQ0 itself is masked now, see lapic_timer)\n",
               (unsigned)irq_manager.handle);

    capability_t tick_counter =
        create_object(tick_counter_handler, NULL, &g_plain_schema);
    job_kprintf(self_job_id,
               "[coreinit] tick_counter object handle: %u, bound to this "
               "core's own LAPIC timer\n",
               (unsigned)tick_counter.handle);
    lapic_timer_bind(tick_counter);

    return JOB_FINISHED;
}

static job_result_t job_smp_bringup(void *ctx, uint64_t self_job_id,
                                    uint64_t *out_block_id)
{
    (void)self_job_id;
    (void)out_block_id;
    smp_job_ctx_t *c = (smp_job_ctx_t *)ctx;

    kprintf("\n[coreinit] === job: bringing up additional cores ===\n");
    kprintf("[coreinit] found %u MADT processor entries:\n",
            (unsigned)c->acpi->cpu_count);
    unsigned usable = 0;
    for (size_t i = 0; i < c->acpi->cpu_count; i++) {
        kprintf("[coreinit]   apic_id=%u %s\n",
                (unsigned)c->acpi->cpus[i].apic_id,
                c->acpi->cpus[i].enabled ? "enabled" : "disabled (present "
                                                       "for hotplug only)");
        if (c->acpi->cpus[i].enabled) {
            usable++;
        }
    }
    kprintf("[coreinit] %u usable core(s) total (including this one)\n",
            usable);

    if (c->is_bsp) {
        smp_bring_up_all(c->acpi, c->bsp_apic_id, c->info, c->memory_manager,
                        c->mutex_manager, c->hw_mutex_id, g_lapic_calibration,
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
    kprintf("\n[coreinit] === job: minting the disk object (real "
            "ATA-backed) ===\n");
    capability_t disk = create_object(disk_handler, NULL, &g_disk_schema);
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
 * genuinely works: ctx here is the SENDING core's apic_id, a plain
 * integer needing no cross-core pointer validity at all. */
static job_result_t job_greet_from_bsp(void *ctx, uint64_t self_job_id,
                                       uint64_t *out_block_id)
{
    (void)self_job_id;
    (void)out_block_id;
    uint64_t sender_apic_id = (uint64_t)(uintptr_t)ctx;
    kprintf("\n[coreinit] === job: greeting received via the cluster "
            "scheduler's cross-node dispatch, sent by apic_id %u -- "
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
 * reasoning. Deliberately standalone: it does not yet consult the
 * cluster scheduler's resource model at all (no sync_kind property
 * exists there yet) -- that integration is real, separate follow-up
 * work once this riskier, lower-level piece (real page table surgery)
 * is proven correct on its own. BSP-only: all cores currently share the
 * same page tables (see kernel.c's own note on this), so touching them
 * here is a single-core demonstration, not yet something safe to do
 * with another core concurrently relying on the same mapping. */
typedef struct {
    uint64_t pml4_phys;
} pagefault_demo_ctx_t;

/* 8MB -- safely above the kernel/coreinit's own load addresses and the
 * memory manager's pre-claimed low regions, safely below
 * CORE_REGION_BASE (32MB) so it can't collide with an AP's region in a
 * multi-core boot, and (this mattered -- see the demo job's own commit
 * history) safely WITHIN this project's actual default QEMU RAM size
 * (~128MB): the original choice of 256MB silently wrote to backing
 * memory that didn't exist, which looks identical to a data-loss bug
 * in the page-fault mechanism itself until you check the actual RAM
 * size this project's own earlier boot logs already reported. */
#define PF_DEMO_REGION 0x800000ull
#define PF_STREAM_PAGE (PF_DEMO_REGION + 0x1000ull)
#define PF_COW_PAGE (PF_DEMO_REGION + 0x2000ull)

static job_result_t job_pagefault_demo(void *ctx, uint64_t self_job_id,
                                       uint64_t *out_block_id)
{
    (void)self_job_id;
    (void)out_block_id;
    pagefault_demo_ctx_t *c = (pagefault_demo_ctx_t *)ctx;

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
}

void coreinit_main(struct init_boot_info *info)
{
    serial_init();
    g_kernel_invoke = info->kernel_invoke;
    g_factory = info->caps.factory;
    proxy_init(g_kernel_invoke, g_factory);

    /* Read this core's own APIC ID directly from the LAPIC's standard,
     * fixed physical address (0xFEE00000 on every x86 system, ACPI
     * discovery or not) as the very first thing, so EVERY line of
     * output from here on is correctly tagged -- without this, the
     * shared-services setup below (which happens before ACPI discovery
     * would otherwise give us this) prints untagged, and on a shared,
     * unlocked UART that reads as attributed to whichever OTHER core
     * happened to interleave a tagged line at the same moment. */
    uint8_t my_apic_id = smp_read_bsp_apic_id(0xFEE00000);
    serial_set_core_tag((int)my_apic_id);

    init_plain_schema();
    init_disk_schema();

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
     * it existing. */
    int is_bsp = (info->bsp_kernel_invoke_addr == 0);

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
                                      &g_mutex_manager_schema);
        memory_manager_init_schema();
        memory_manager = create_object(memory_manager_handler, NULL,
                                       &g_memory_manager_schema);
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
        g_hw_mutex_manager = mutex_manager;
        g_hw_mutex_id = id_out[0];
        kprintf("[coreinit] hardware mutex created, id %u\n",
                (unsigned)g_hw_mutex_id);
    } else {
        kprintf("\n[coreinit] === discovered shared services from the BSP "
                "===\n");
        /* The PROXY objects we're about to create are validated by the
         * LOCAL kernel against the schema THEY were created with, not
         * against whatever the real remote object implements -- so this
         * node needs its own populated copy of the same schema struct
         * (every node has one, being byte-identical compiled code) even
         * though it will never run the real handler logic locally. */
        mutex_manager_build_schema();
        remote_invoke_fn_t bsp_invoke =
            (remote_invoke_fn_t)(uintptr_t)info->bsp_kernel_invoke_addr;
        capability_t remote_mutex_manager;
        remote_mutex_manager.handle = info->mutex_manager_handle;

        /* A LOCAL proxy, not the raw remote pair -- from here on,
         * g_hw_mutex_manager is an ordinary local capability, invoked
         * through the ordinary local g_kernel_invoke, same as anything
         * else. hw_mutex_lock/unlock below don't need to know or care
         * that it's actually remote. */
        g_hw_mutex_manager = create_proxy(remote_mutex_manager, bsp_invoke,
                                          &g_mutex_manager_schema);
        g_hw_mutex_invoke = g_kernel_invoke;
        g_hw_mutex_id = info->hardware_mutex_id;
        kprintf("[coreinit] mutex_manager reachable via local proxy handle "
                "%u, hardware mutex id %u\n",
                (unsigned)g_hw_mutex_manager.handle, (unsigned)g_hw_mutex_id);
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
                                          &g_cluster_scheduler_schema);
        kprintf("[coreinit] cluster_scheduler handle %u\n",
                (unsigned)cluster_scheduler.handle);
    } else {
        capability_t remote_cluster_scheduler;
        remote_cluster_scheduler.handle = info->cluster_scheduler_handle;
        remote_invoke_fn_t bsp_invoke =
            (remote_invoke_fn_t)(uintptr_t)info->bsp_kernel_invoke_addr;
        cluster_scheduler_build_schema(); /* same reasoning as
                                           * mutex_manager_build_schema
                                           * above */
        cluster_scheduler = create_proxy(remote_cluster_scheduler, bsp_invoke,
                                         &g_cluster_scheduler_schema);
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
         * fake frame with IF=1) -- see pic_remap's own note on why
         * this specific ordering matters. irq_init (called later, via
         * job_timer_bringup) redoes both of these, harmlessly. */
        pic_remap();
        irq_setup_idt_only();
        run_job_exec_test();
        run_job_exec_priority_test();
    }

    /* ACPI discovery happens once, here, since both job_timer_bringup
     * (needs the local APIC's MMIO base) and job_smp_bringup (needs the
     * full core list) depend on it -- acpi and bsp_apic_id both outlive
     * this function (it never returns) as ordinary local variables,
     * same as everything else coreinit_main sets up before handing off.
     * bsp_apic_id itself was already read, early, above -- reused here
     * rather than re-read, "bsp_apic_id" just means "this core's own
     * APIC ID", the name predates this also running on APs. */
    static acpi_info_t acpi;
    uint8_t bsp_apic_id = my_apic_id;
    int have_acpi = acpi_discover(&acpi, info->early_rsdp_phys);
    if (have_acpi) {
        kprintf("[coreinit] this core's own APIC ID: %u\n",
                (unsigned)bsp_apic_id);
    } else {
        kprintf("[coreinit] ACPI discovery failed -- no timer bring-up or "
                "further cores possible this boot\n");
    }

    /* Cross-node scheduling setup: register the identical job-type
     * vocabulary every node registers, tell the client layer how to
     * reach the cluster scheduler, and join the cluster. All of this is
     * meaningful even before ACPI succeeds (job-type registration
     * doesn't depend on it), so it happens unconditionally; only
     * join_node needs a real apic_id, which defaults sensibly to 0 if
     * ACPI discovery somehow failed. */
    register_job_types();
    uint64_t my_region_base = is_bsp ? BSP_MAILBOX_REGION_BASE
                                     : (info->own_mailbox_base - CORE_MAILBOX_OFFSET);
    /* Always the LOCAL kernel_invoke now -- cluster_scheduler is either
     * genuinely local (BSP) or a local proxy (AP), so there's no more
     * "which invoke function" special-casing needed here at all. */
    scheduler_client_init(g_kernel_invoke, cluster_scheduler, bsp_apic_id);

    uint64_t reg_words[4] = {bsp_apic_id, CLUSTER_ARCH_X86_64, my_region_base,
                             cross_core_signal_my_target()};
    uint8_t reg_req[48];
    size_t reg_req_len = capnp_build_flat(reg_req, sizeof(reg_req), reg_words, 4);
    g_kernel_invoke(cluster_scheduler, 0, reg_req, reg_req_len, NULL, 0, NULL);

    /* Join is identity only -- resources are this node's own to
     * declare afterward, the same declare_resource path everything
     * else (the shared serial resource just below, a job's own
     * peripheral demo, ...) already uses, not something join hands out
     * automatically. cpu_core mirrors what every node has always
     * implicitly provided; mmu is declared only if this node actually
     * has one (see arch_mmu.h) -- absence is the normal, expected case
     * on some future non-x86_64 node, not an error here. */
    uint64_t my_cpu_core = scheduler_declare_resource(
        RESOURCE_KIND_CPU_CORE, CLUSTER_ARCH_X86_64, 0, RESOURCE_SYNC_TRANSFER);
    kprintf("[coreinit] declared this node's own cpu_core resource, id %u\n",
           (unsigned)my_cpu_core);
    if (arch_has_mmu()) {
        uint64_t my_mmu = scheduler_declare_resource(
            RESOURCE_KIND_MMU, CLUSTER_ARCH_X86_64, 0, RESOURCE_SYNC_TRANSFER);
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
        resource_object_build_schema(); /* same reasoning as
                                         * mutex_manager_build_schema /
                                         * cluster_scheduler_build_schema
                                         * above */
        g_serial_resource =
            create_proxy(remote_serial, bsp_invoke, &g_resource_schema);
        kprintf("[coreinit] serial resource reachable via local proxy "
                "handle 0x%X\n",
                (unsigned)g_serial_resource.handle);
    }

    kprintf("\n[coreinit] === submitting jobs, then handing off to the "
            "scheduler ===\n");

    if (have_acpi) {
        static timer_job_ctx_t timer_ctx;
        timer_ctx.acpi = &acpi;
        timer_ctx.is_bsp = is_bsp;
        timer_ctx.discovered_lapic_calibration = info->lapic_calibration_count;
        scheduler_submit(JOB_TYPE_TIMER_BRINGUP, &timer_ctx, 100, 0, RESOURCE_KIND_NONE, 0);

        static smp_job_ctx_t smp_ctx;
        smp_ctx.acpi = &acpi;
        smp_ctx.bsp_apic_id = bsp_apic_id;
        smp_ctx.info = info;
        smp_ctx.memory_manager = memory_manager;
        smp_ctx.mutex_manager = mutex_manager;
        smp_ctx.hw_mutex_id = g_hw_mutex_id;
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
    }

    kprintf("\n[coreinit] TODO: load securinit as the next stage and hand "
            "off -- authorization/wrapping starts there, not here\n");

    scheduler_run();
}
