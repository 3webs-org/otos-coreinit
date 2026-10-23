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
#define GICD_ICENABLER      0x0180  /* Interrupt Clear-Enable Registers */
#define GICD_IPRIORITYR     0x0400  /* Interrupt Priority Registers (4 interrupts per register) */
#define GICD_IROUTER        0x6000  /* Interrupt Routing Registers (GICv3, 8 bytes per interrupt) */
#define GICC_CTLR           0x00    /* GICv2 CPU interface */
#define GICC_PMR            0x04
#define GICC_IAR            0x0c
#define GICC_EOIR           0x10
#define GICR_TYPER          0x0008
#define GICR_WAKER          0x0014
#define GICR_SGI_BASE       0x10000
#define GICR_IGROUPR0       (GICR_SGI_BASE + 0x80)
#define GICR_ISENABLER0     (GICR_SGI_BASE + 0x100)
#define GICR_ICENABLER0     (GICR_SGI_BASE + 0x180)
#define GICR_IPRIORITYR     (GICR_SGI_BASE + 0x400)
#define GICR_FRAME_SIZE     0x20000

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
 * The EL1 vector's C dispatcher consults this table after GIC acknowledge. */
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
static volatile uint8_t *g_gicr_region = NULL;
static uint64_t g_gicr_region_size = 0;
static volatile uint8_t *g_gicr_base = NULL;

/* Physical timer (PPI 14) is INTID 30. */
#define ARM_TIMER_IRQ 30
static uint64_t g_tick_count = 0;
extern char irq_aarch64_vectors[];

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

    /* Firmware may have left device IRQs enabled (EFI-stub chainloading
     * leaves more pending sources than the xnu path). Clear every enable
     * bank before we unmask the CPU: only irq_install_handler may re-enable
     * an INTID with a real callback. A pending level IRQ without a handler
     * otherwise livelocks this core immediately on EOI. */
    mmio_write32((volatile void *)(g_gicd_base + GICD_CTLR), 0);
    for (int i = 0; i < 8; i++) {
        mmio_write32((volatile void *)(g_gicd_base + GICD_ICENABLER + i * 4),
                     0xffffffffu);
    }

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
        /* Enable Group 0 and Group 1. AckCtl (bit 2) lets IAR read both:
         * without it QEMU reports 1022 for every Group 1 IRQ. */
        mmio_write32((volatile void *)(g_gicc_base + GICC_CTLR), 7);
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
 * Called by the EL1 vector's C dispatcher. */
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
 * Called by the EL1 vector's C dispatcher for non-spurious IDs. */
void irq_aarch64_eoi(uint32_t irq_id)
{
    if (g_gicc_base) {
        mmio_write32((volatile void *)(g_gicc_base + GICC_EOIR), irq_id);
        return;
    }
    write_sysreg(ICC_EOIR1_EL1, (uint64_t)irq_id);
}

/* Called from the EL1 vector with all GPRs and ELR/SPSR saved. For a
 * GICv2 SGI the upper IAR bits carry the source CPU ID; EOI must receive
 * that original value, not only INTID[9:0]. */
void irq_aarch64_dispatch(void)
{
    uint32_t raw = irq_aarch64_acknowledge();
    uint32_t id = raw & 0x3ffu;

    if (irq_is_spurious(id)) {
        return;
    }

    if (id == ARM_TIMER_IRQ) {
        g_tick_count++;
    }
    if (id < MAX_IRQ_HANDLERS) {
        void (*handler)(void) = (void (*)(void))g_irq_handlers[id];
        if (handler) {
            handler();
        }
        capability_t target = g_irq_targets[id];
        if (target.handle != 0 && g_kernel_invoke) {
            uint64_t word = id;
            uint8_t msg[24];
            size_t len = capnp_build_flat(msg, sizeof(msg), &word, 1);
            g_kernel_invoke(target, 0, msg, len, NULL, 0, NULL);
        }
    }
    irq_aarch64_eoi(raw);
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

    /* Install the EL1 table before enabling any source. The earlier job
     * switch may restore DAIF.I=0 on a fresh job, so retaining firmware's
     * VBAR_EL1 even briefly after configuring the GIC is unsafe. */
    __asm__ volatile("msr vbar_el1, %0\n\tisb" ::
                     "r"((uintptr_t)irq_aarch64_vectors) : "memory");
    gic_distributor_init();
    if (!g_gicc_base) {
        /* GICv3 routes SGIs and PPIs through the redistributor of this
         * affinity, not the shared distributor. Fail closed if firmware
         * supplied no usable GICR region. */
        uint64_t mpidr;
        __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
        uint32_t affinity = ((uint32_t)(mpidr >> 32) & 0xffu) << 24 |
                            ((uint32_t)mpidr & 0xffffffu);
        g_gicr_base = NULL;
        for (uint64_t off = 0; g_gicr_region &&
             off + GICR_FRAME_SIZE <= g_gicr_region_size;
             off += GICR_FRAME_SIZE) {
            volatile uint8_t *frame = g_gicr_region + off;
            uint64_t typer = mmio_read64((const volatile void *)(frame + GICR_TYPER));
            if ((uint32_t)(typer >> 32) == affinity) {
                g_gicr_base = frame;
                break;
            }
            if (typer & (1ull << 4)) {
                break; /* Last redistributor in this contiguous region. */
            }
        }
        if (!g_gicr_base) {
            kprintf("[coreinit] irq_core_init: GICv3 redistributor not found "
                    "for affinity 0x%x; IRQs remain masked\n", affinity);
            return;
        }
        uint32_t waker = mmio_read32((const volatile void *)(g_gicr_base + GICR_WAKER));
        mmio_write32((volatile void *)(g_gicr_base + GICR_WAKER),
                     waker & ~(1u << 1)); /* ProcessorSleep = 0 */
        int ready = 0;
        for (unsigned i = 0; i < 1000000; i++) {
            if (!(mmio_read32((const volatile void *)(g_gicr_base + GICR_WAKER)) &
                  (1u << 2))) { /* ChildrenAsleep = 0 */
                ready = 1;
                break;
            }
        }
        if (!ready) {
            kprintf("[coreinit] irq_core_init: GICv3 redistributor did not wake\n");
            return;
        }
        mmio_write32((volatile void *)(g_gicr_base + GICR_ICENABLER0),
                     0xffffffffu);
        mmio_write32((volatile void *)(g_gicr_base + GICR_IGROUPR0),
                     0xffffffffu);
        for (int i = 0; i < 8; i++) {
            mmio_write32((volatile void *)(g_gicr_base + GICR_IPRIORITYR + i * 4),
                         0x80808080u);
        }
    }
    gic_cpu_interface_init();
    __asm__ volatile("msr daifclr, #2" ::: "memory");
    kprintf("[coreinit] irq_core_init: EL1 vectors and GIC initialized; "
            "IRQs enabled\n");
}

void irq_install_handler(int vector, void *handler_addr)
{
    /* Bounds check: only MAX_IRQ_HANDLERS vectors are supported */
    if (vector < 0 || vector >= MAX_IRQ_HANDLERS) {
        kprintf("[coreinit]   irq: WARNING: irq_install_handler(%d) out of range [0, %d)\n",
                vector, MAX_IRQ_HANDLERS);
        return;
    }

    /* The installed EL1 vector dispatches to this callback after
     * acknowledging its GIC interrupt ID. */
    g_irq_handlers[vector] = handler_addr;

    /* SGIs/PPIs (<32) are per-core redistributor registers on GICv3;
     * GICv2 uses the distributor's bank 0. SPIs (>=32) use GICD on both. */
    if (g_gicr_base && vector < 32) {
        mmio_write32((volatile void *)(g_gicr_base + GICR_ISENABLER0),
                     1u << vector);
        kprintf("[coreinit]   irq: installed handler for vector %d at %p, "
                "enabled in GICR\n", vector, handler_addr);
    } else if (g_gicd_base) {
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

void irq_aarch64_set_redistributor_region(uint64_t base, uint64_t size)
{
    g_gicr_region = (volatile uint8_t *)(uintptr_t)base;
    g_gicr_region_size = size;
}

/* The irq_manager object's handler and schema. This is the object-model
 * interface for binding IRQ targets -- method 0 takes {irq: u64, target: u64}
 * and binds the target capability to be invoked (method 0, message = IRQ number)
 * every time that IRQ fires. This is identical to the x86_64 implementation. */


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
