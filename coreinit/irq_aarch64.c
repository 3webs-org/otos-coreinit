#include "irq.h"
#include "mmio.h"
#include "serial.h"
#include "capnp_build.h"
#include "schema.h"
#include <stdint.h>
#include <stddef.h>

/* GICv3 Distributor (GICD) register offsets from the distributor base.
 * The distributor is MMIO-mapped and controls global interrupt routing,
 * priority, and enable state. Base address is discovered from the device
 * tree by topology_aarch64.c and stored in arch_opaque[1]. */
#define GICD_CTLR           0x0000  /* Distributor Control Register */
#define GICD_IGROUPR        0x0080  /* Interrupt Group Registers (32 interrupts per register) */
#define GICD_ISENABLER      0x0100  /* Interrupt Set-Enable Registers */
#define GICD_IPRIORITYR     0x0400  /* Interrupt Priority Registers (4 interrupts per register) */
#define GICD_IROUTER        0x6000  /* Interrupt Routing Registers (GICv3, 8 bytes per interrupt) */
#define GICC_CTLR           0x00    /* GICv2 CPU interface */
#define GICC_PMR            0x04
#define GICC_IAR            0x0c
#define GICC_EOIR           0x10

/* GICD_CTLR bits */
#define GICD_CTLR_ENABLEGRP0    (1U << 0)  /* Enable Group 0 interrupts */
#define GICD_CTLR_ENABLEGRP1NS  (1U << 1)  /* Enable Group 1 Non-Secure interrupts */

/* GICv3 CPU Interface system registers. Unlike GICv2 (which used MMIO for
 * the CPU interface), GICv3 uses system registers accessed via MRS/MSR.
 * These are per-core registers that control interrupt delivery to THIS core. */
#define ICC_SRE_EL1_SRE     (1U << 0)  /* System Register Enable */

/* Maximum number of IRQ handlers we track. This is deliberately small --
 * only the timer and a few device interrupts need handlers in coreinit.
 * The full GIC supports 1020 interrupts (32-1019 are SPIs, 0-31 are SGIs/PPIs),
 * but we only need a handful. */
#define MAX_IRQ_HANDLERS 32

/* IRQ line -> handler address. Zero means "no handler installed yet".
 * This table will be consulted by the exception vector table (assembly,
 * a later unit) to dispatch interrupts. For now, irq_install_handler()
 * records the handler here and documents that the vector table does not
 * exist yet -- this is the "record intent" design from the task spec. */
static void *g_irq_handlers[MAX_IRQ_HANDLERS];

/* IRQ line -> capability to invoke (method 0, message = IRQ number) on
 * every fire. handle 0 doubles as "unbound", same convention as everywhere
 * else caller/target identity is tracked in this project. */
static capability_t g_irq_targets[MAX_IRQ_HANDLERS];

static int (*g_kernel_invoke)(capability_t target, uint32_t method_id,
                              const void *msg, size_t len, void *response,
                              size_t response_cap, size_t *out_response_len);

/* GIC distributor base address, discovered from the device tree by
 * topology_aarch64.c. Zero means "not yet configured" -- irq_core_init()
 * with base==0 reports and returns rather than faulting on a null pointer
 * dereference. This mirrors the design in serial_aarch64.c. */
static volatile uint8_t *g_gicd_base = NULL;
static volatile uint8_t *g_gicc_base = NULL;

/* Tick count incremented by the timer interrupt handler (once it exists).
 * Exposed via irq_get_tick_count() for LAPIC timer calibration on x86_64;
 * on aarch64 the generic timer has its own frequency source, but we keep
 * this for API compatibility. */
static uint64_t g_tick_count = 0;

/* Read a GICv3 CPU interface system register. These are accessed via MRS
 * (Move to Register from System register), not MMIO. The register name is
 * a compile-time constant, so this is a macro not a function. */
#define read_sysreg(reg) ({ \
    uint64_t _val; \
    __asm__ volatile("mrs %0, " #reg : "=r"(_val)); \
    _val; \
})

/* Write a GICv3 CPU interface system register via MSR (Move to System
 * register from Register). */
#define write_sysreg(reg, val) do { \
    uint64_t _val = (val); \
    __asm__ volatile("msr " #reg ", %0" :: "r"(_val)); \
} while (0)

/* Initialize the GIC distributor: enable interrupt groups, set all
 * interrupts to Group 1 (non-secure), and set default priorities.
 * This does NOT enable individual interrupts -- that happens later
 * when a handler is installed. */
static void gic_distributor_init(void)
{
    if (!g_gicd_base) {
        kprintf("[coreinit]   irq: GIC distributor base is NULL, skipping init\n");
        return;
    }

    /* Disable the distributor while we configure it */
    mmio_write32((volatile void *)(g_gicd_base + GICD_CTLR), 0);

    /* Set all interrupts to Group 1 (non-secure). IGROUPR is a bitmap,
     * 32 interrupts per register. We configure the first 8 registers
     * (256 interrupts), which covers all SPIs on typical systems. */
    for (int i = 0; i < 8; i++) {
        mmio_write32((volatile void *)(g_gicd_base + GICD_IGROUPR + i * 4), 0xFFFFFFFF);
    }

    /* Set all interrupt priorities to a middle value (0x80). Lower values
     * are higher priority. IPRIORITYR packs 4 interrupts per 32-bit register,
     * one byte each. We configure 256 interrupts (64 registers). */
    for (int i = 0; i < 64; i++) {
        mmio_write32((volatile void *)(g_gicd_base + GICD_IPRIORITYR + i * 4), 0x80808080);
    }

    /* Enable Group 0 and Group 1 (non-secure) interrupts in the distributor */
    mmio_write32((volatile void *)(g_gicd_base + GICD_CTLR),
                 GICD_CTLR_ENABLEGRP0 | GICD_CTLR_ENABLEGRP1NS);

    kprintf("[coreinit]   irq: GIC distributor initialized at %p\n", (void *)g_gicd_base);
}

/* Initialize the GICv3 CPU interface for THIS core. This configures the
 * system registers that control interrupt delivery to this core:
 * - Enable system register access (ICC_SRE_EL1)
 * - Set priority mask to allow all interrupts (ICC_PMR_EL1)
 * - Enable Group 1 interrupt delivery (ICC_IGRPEN1_EL1)
 *
 * This is per-core state -- each core that wants to receive interrupts
 * must call this during its own bring-up. */
static void gic_cpu_interface_init(void)
{
    if (g_gicc_base) {
        /* GICv2's CPU interface is MMIO, not ICC_* system registers.
         * Writing ICC_SRE_EL1 on QEMU virt's default GICv2 raises an
         * Undefined Instruction exception. */
        mmio_write32((volatile void *)(g_gicc_base + GICC_PMR), 0xff);
        mmio_write32((volatile void *)(g_gicc_base + GICC_CTLR), 1);
        kprintf("[coreinit]   irq: GICv2 CPU interface initialized\n");
        return;
    }

    /* Enable system register access for the GICv3 CPU interface. */
    write_sysreg(ICC_SRE_EL1, ICC_SRE_EL1_SRE);

    /* Set the priority mask to 0xFF (lowest priority), which allows ALL
     * interrupts to be delivered. If this is left at 0 (the reset value),
     * NO interrupts will ever be delivered -- this is the most common
     * mistake when bringing up GICv3. */
    write_sysreg(ICC_PMR_EL1, 0xFF);

    /* Enable Group 1 interrupt delivery to this core. Group 1 is used for
     * non-secure interrupts, which is what we configured in the distributor. */
    write_sysreg(ICC_IGRPEN1_EL1, 1);

    /* Set binary point to 0 (all 8 priority bits used for preemption).
     * This is the default, but we set it explicitly for clarity. */
    write_sysreg(ICC_BPR1_EL1, 0);

    kprintf("[coreinit]   irq: GIC CPU interface initialized\n");
}

/* GICv3 spurious interrupt ID range. When ICC_IAR1_EL1 returns a value in
 * this range (1020-1023, or 0x3FC-0x3FF), it means "no interrupt is pending"
 * or the interrupt was withdrawn before acknowledgment. These IDs MUST NOT
 * be written to ICC_EOIR1_EL1 -- doing so corrupts the active-priority stack
 * and can prevent future interrupts from being delivered. */
#define GIC_SPURIOUS_ID_MIN 1020
#define GIC_SPURIOUS_ID_MAX 1023

/* Check if an interrupt ID is spurious (must not be EOI'd). */
static inline int irq_is_spurious(uint32_t irq_id)
{
    return (irq_id >= GIC_SPURIOUS_ID_MIN && irq_id <= GIC_SPURIOUS_ID_MAX);
}

/* Acknowledge an interrupt and return its ID. This reads ICC_IAR1_EL1,
 * which atomically marks the highest-priority pending interrupt as active
 * and returns its ID. The interrupt will remain active (and block equal-
 * or-lower priority interrupts) until irq_aarch64_eoi() is called.
 *
 * If no interrupt is pending, this returns a spurious ID (1020-1023).
 * The caller MUST check irq_is_spurious() before processing the interrupt
 * or calling irq_aarch64_eoi().
 *
 * This function is not yet called -- the exception vector table (assembly,
 * a later unit) will call it on entry to the IRQ handler. For now, it is
 * defined here so the gap is visible and documented. */
uint32_t irq_aarch64_acknowledge(void)
{
    if (g_gicc_base) {
        return mmio_read32((const volatile void *)(g_gicc_base + GICC_IAR));
    }
    return (uint32_t)read_sysreg(ICC_IAR1_EL1);
}

/* Signal end-of-interrupt for the given IRQ ID. This writes ICC_EOIR1_EL1,
 * which deactivates the interrupt and allows the GIC to deliver it (or any
 * equal-or-lower priority interrupt) again.
 *
 * CRITICAL: Do NOT call this for spurious IDs (1020-1023). Doing so corrupts
 * the active-priority stack and can prevent future interrupts from being
 * delivered. Always check irq_is_spurious() first.
 *
 * This function is not yet called -- the exception vector table (assembly,
 * a later unit) will call it on exit from the IRQ handler, after checking
 * for spurious IDs. For now, it is defined here so the gap is visible and
 * documented. */
void irq_aarch64_eoi(uint32_t irq_id)
{
    if (g_gicc_base) {
        mmio_write32((volatile void *)(g_gicc_base + GICC_EOIR), irq_id);
        return;
    }
    write_sysreg(ICC_EOIR1_EL1, (uint64_t)irq_id);
}

void irq_core_init(int (*kernel_invoke_fn)(capability_t, uint32_t, const void *,
                                           size_t, void *, size_t, size_t *))
{
    g_kernel_invoke = kernel_invoke_fn;

    /* Initialize handler and target tables to zero (no handlers, no bindings) */
    for (int i = 0; i < MAX_IRQ_HANDLERS; i++) {
        g_irq_handlers[i] = NULL;
        g_irq_targets[i].handle = 0;
    }

    /* Check if the GIC base address has been set. If not, we can't configure
     * the GIC, so report and return. This is not an error -- it just means
     * topology_discover() hasn't run yet, or the device tree didn't declare
     * a GIC. The caller can set the base later via irq_aarch64_set_base()
     * and call this again, or proceed without interrupts. */
    if (!g_gicd_base) {
        kprintf("[coreinit] irq_core_init: GIC base not set (topology not run yet?), "
                "skipping GIC configuration\n");
        return;
    }

    /* Initialize the GIC distributor (global state) and CPU interface
     * (per-core state). The distributor is initialized once by the BSP;
     * the CPU interface must be initialized by each core. */
    gic_distributor_init();
    gic_cpu_interface_init();

    /* The exception-vector table has not been installed yet. Keep DAIF.I
     * masked even after a handler is registered; otherwise the first timer
     * interrupt vectors into firmware's stale handler instead of ours. */
    kprintf("[coreinit] irq_core_init: GIC initialized; IRQs masked until "
            "exception vectors are installed\n");
}

void irq_install_handler(int vector, void *handler_addr)
{
    /* Bounds check: only MAX_IRQ_HANDLERS vectors are supported */
    if (vector < 0 || vector >= MAX_IRQ_HANDLERS) {
        kprintf("[coreinit]   irq: WARNING: irq_install_handler(%d) out of range [0, %d)\n",
                vector, MAX_IRQ_HANDLERS);
        return;
    }

    /* Record the handler address in the table. The exception vector table
     * (assembly, a later unit) will dispatch through this table once it
     * exists. For now, this just records intent -- the handler is stored
     * but not yet callable, because the vector table does not exist.
     *
     * This is deliberate: irq_install_handler() is part of the arch-neutral
     * IRQ surface, and must exist and be callable even before the full
     * interrupt path is wired up. A function that silently does nothing
     * would be worse than one that records intent and says so. */
    g_irq_handlers[vector] = handler_addr;

    /* If the GIC is configured, enable this interrupt in the distributor.
     * ISENABLER is a set-enable register (write 1 to enable, write 0 is
     * ignored), with 32 interrupts per 32-bit register. */
    if (g_gicd_base) {
        uint32_t reg_offset = GICD_ISENABLER + (vector / 32) * 4;
        uint32_t bit = 1U << (vector % 32);
        mmio_write32((volatile void *)(g_gicd_base + reg_offset), bit);
        kprintf("[coreinit]   irq: installed handler for vector %d at %p, enabled in GIC\n",
                vector, handler_addr);
    } else {
        kprintf("[coreinit]   irq: installed handler for vector %d at %p (GIC not configured, "
                "not enabled in hardware)\n", vector, handler_addr);
    }
}

uint64_t irq_get_tick_count(void)
{
    return g_tick_count;
}

/* Set the GIC distributor base address. This is called by the aarch64
 * bring-up code after topology_discover() has parsed the device tree.
 * The base address is stored in topology_t.arch_opaque[1].
 *
 * This function is not declared in a header -- the bring-up code will
 * forward-declare it where needed. This mirrors the design in
 * serial_aarch64.c (serial_aarch64_set_base). */
void irq_aarch64_set_base(uint64_t base)
{
    g_gicd_base = (volatile uint8_t *)(uintptr_t)base;
    kprintf("[coreinit]   irq: GIC distributor base set to 0x%llX\n",
           (unsigned long long)base);
}

void irq_aarch64_set_cpu_base(uint64_t base)
{
    g_gicc_base = (volatile uint8_t *)(uintptr_t)base;
}

/* The irq_manager object's handler and schema. This is the object-model
 * interface for binding IRQ targets -- method 0 takes {irq: u64, target: u64}
 * and binds the target capability to be invoked (method 0, message = IRQ number)
 * every time that IRQ fires. This is identical to the x86_64 implementation. */

static const struct_schema_t irq_structs[] = {
    {2, 0, SCHEMA_IDX_NONE}, /* {irq: u64, target: u64} */
};
static const method_schema_t irq_methods[] = {
    {0, 0, SCHEMA_IDX_NONE},
};
static const object_schema_t irq_objects[] = {{0, 1}};
const schema_table_t g_irq_manager_table = {
    irq_structs, 1, NULL, 0, irq_methods, 1, irq_objects, 1};

void irq_manager_handler(void *data, uint32_t method_id, const void *msg,
                        size_t len, void *response, size_t response_cap,
                        size_t *out_response_len)
{
    (void)data;
    (void)method_id;
    (void)len;
    (void)response;
    (void)response_cap;
    *out_response_len = 0;

    const uint64_t *words = capnp_read_flat(msg);
    uint64_t irq = words[0];
    capability_t target;
    target.handle = words[1];

    if (irq >= MAX_IRQ_HANDLERS) {
        kprintf("[coreinit]   irq_manager: bind refused, IRQ %llu out of range\n",
               (unsigned long long)irq);
        return;
    }
    g_irq_targets[irq] = target;
    kprintf("[coreinit]   irq_manager: bound IRQ %llu to capability handle %llu\n",
           (unsigned long long)irq, (unsigned long long)target.handle);
}
