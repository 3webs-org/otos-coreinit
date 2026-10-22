#include "lapic_timer.h"
#include "capnp_build.h"
#include "irq.h"
#include "mmio.h"
#include "serial.h"

#define LAPIC_REG_EOI 0x0B0
#define LAPIC_REG_SVR 0x0F0
#define LAPIC_REG_LVT_TIMER 0x320
#define LAPIC_REG_INITIAL_COUNT 0x380
#define LAPIC_REG_CURRENT_COUNT 0x390
#define LAPIC_REG_DIVIDE_CONFIG 0x3E0

#define LVT_MASKED (1u << 16)
#define LVT_MODE_PERIODIC (1u << 17)
#define SVR_APIC_SOFTWARE_ENABLE (1u << 8)
#define SVR_SPURIOUS_VECTOR 0xFF /* conventional, unused elsewhere in
                                  * this codebase's vector space */

#define LAPIC_TIMER_VECTOR 0x22
#define DIVIDE_BY_16 0x3

/* clang/gcc's x86 interrupt-attribute calling convention needs this
 * exact frame shape declared -- same requirement as irq.c's own copy,
 * duplicated here rather than shared since it's a compiler-ABI detail
 * with no actual behavior to keep in sync. */
struct interrupt_frame {
    uint64_t ip;
    uint64_t cs;
    uint64_t flags;
    uint64_t sp;
    uint64_t ss;
};

static uint32_t g_lapic_base;
static uint64_t g_ticks = 0;
static capability_t g_timer_target;
static int (*g_kernel_invoke)(capability_t, uint32_t, const void *, size_t,
                              void *, size_t, size_t *);

static uint32_t lapic_read(uint32_t reg)
{
    return mmio_read32((const volatile void *)(uintptr_t)(g_lapic_base + reg));
}

static void lapic_write(uint32_t reg, uint32_t val)
{
    mmio_write32((volatile void *)(uintptr_t)(g_lapic_base + reg), val);
}

__attribute__((interrupt)) static void isr_lapic_timer(struct interrupt_frame *frame)
{
    (void)frame;
    g_ticks++;
    if (g_timer_target.handle != 0) {
        uint64_t word = g_ticks;
        uint8_t msg[24];
        size_t msg_len = capnp_build_flat(msg, sizeof(msg), &word, 1);
        g_kernel_invoke(g_timer_target, 0, msg, msg_len, NULL, 0, NULL);
    }
    lapic_write(LAPIC_REG_EOI, 0); /* any value acknowledges a LAPIC-local
                                    * interrupt -- distinct from, and
                                    * unrelated to, the legacy 8259 PIC's
                                    * own EOI port */
}

uint32_t lapic_timer_calibrate(uint32_t lapic_base)
{
    g_lapic_base = lapic_base;

    lapic_write(LAPIC_REG_DIVIDE_CONFIG, DIVIDE_BY_16);
    lapic_write(LAPIC_REG_LVT_TIMER, LVT_MASKED);

    uint64_t start = irq_get_tick_count();
    while (irq_get_tick_count() == start) {
    }
    lapic_write(LAPIC_REG_INITIAL_COUNT, 0xFFFFFFFFu);
    uint64_t measure_until = irq_get_tick_count() + 5;
    while (irq_get_tick_count() < measure_until) {
    }
    uint32_t remaining = lapic_read(LAPIC_REG_CURRENT_COUNT);
    uint32_t elapsed = 0xFFFFFFFFu - remaining;
    uint32_t count_per_10ms = elapsed / 5;

    kprintf("[coreinit]   lapic_timer: calibrated, %u counts per 10ms "
            "(divide-by-16)\n",
            (unsigned)count_per_10ms);

    irq_mask_irq0(); /* the PIT has done its one and only job, for the
                      * one and only core that can ever do it -- see
                      * lapic_timer.h's own note on why an AP could never
                      * reach this function usefully */
    return count_per_10ms;
}

void lapic_timer_start(
    uint32_t lapic_base, uint32_t count_per_10ms,
    int (*kernel_invoke_fn)(capability_t, uint32_t, const void *, size_t,
                           void *, size_t, size_t *))
{
    g_lapic_base = lapic_base;
    g_kernel_invoke = kernel_invoke_fn;
    g_timer_target.handle = 0;

    irq_install_gate(LAPIC_TIMER_VECTOR, (void *)isr_lapic_timer);

    /* Software-enable the Local APIC itself -- distinct from, and in
     * addition to, EFLAGS.IF (the "sti" below). This bit gates ALL
     * LAPIC-generated interrupt delivery (timer, IPIs, everything),
     * regardless of EFLAGS.IF. The BSP's LAPIC happens to come up with
     * this already set (an observed QEMU/BSP-reset convention, not
     * something to rely on), but an AP's LAPIC goes through an INIT-IPI
     * reset as part of bring-up (see smp.c's send_init_sipi_sipi) which
     * resets it back to disabled -- without this write, an AP's timer
     * (and any cross_core_signal_send targeting it) silently never
     * fires, with no error anywhere to point at why. Writing it
     * unconditionally on every call, BSP included, means this doesn't
     * depend on that undocumented default at all. */
    lapic_write(LAPIC_REG_SVR, SVR_SPURIOUS_VECTOR | SVR_APIC_SOFTWARE_ENABLE);

    lapic_write(LAPIC_REG_DIVIDE_CONFIG, DIVIDE_BY_16);
    lapic_write(LAPIC_REG_INITIAL_COUNT, count_per_10ms);
    lapic_write(LAPIC_REG_LVT_TIMER, LAPIC_TIMER_VECTOR | LVT_MODE_PERIODIC);

    /* Idempotent if irq_init already did this (the BSP's path) --
     * necessary if it didn't (an AP, which only ever calls
     * irq_setup_idt_only, never irq_init, and so never otherwise enables
     * interrupts at all). */
    __asm__ volatile("sti");

    kprintf("[coreinit]   lapic_timer: running independently at ~100Hz, "
            "vector 0x%X (calibration: %u counts/10ms)\n",
            LAPIC_TIMER_VECTOR, (unsigned)count_per_10ms);
}

void lapic_timer_calibrate_and_start(
    uint32_t lapic_base,
    int (*kernel_invoke_fn)(capability_t, uint32_t, const void *, size_t,
                           void *, size_t, size_t *))
{
    /* Retained for the BSP's own convenience -- it's the one core that
     * can validly do both steps back to back. */
    uint32_t count = lapic_timer_calibrate(lapic_base);
    lapic_timer_start(lapic_base, count, kernel_invoke_fn);
}

void lapic_timer_bind(capability_t target)
{
    g_timer_target = target;
}

uint64_t lapic_timer_get_ticks(void)
{
    return g_ticks;
}

void lapic_timer_eoi(void)
{
    lapic_write(LAPIC_REG_EOI, 0);
}

void lapic_timer_install_isr(void *isr)
{
    irq_install_gate(LAPIC_TIMER_VECTOR, isr);
}

void lapic_timer_restore_normal_isr(void)
{
    irq_install_gate(LAPIC_TIMER_VECTOR, (void *)isr_lapic_timer);
}
