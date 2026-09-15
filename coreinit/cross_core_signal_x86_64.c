/* x86_64 backend for cross_core_signal.h. Everything x86-specific
 * about cross-core preemption signaling lives in this one file --
 * porting to another architecture means writing a new file matching
 * cross_core_signal.h's contract, not touching anything that includes
 * that header. */
#include "cross_core_signal.h"
#include "irq.h"
#include "mmio.h"

/* Same fixed physical address used directly elsewhere in this codebase
 * (see coreinit.c's own smp_read_bsp_apic_id(0xFEE00000) call) rather
 * than depending on lapic_timer.c's private, timer-specific copy of
 * this same base -- this file is deliberately self-sufficient. */
#define LAPIC_BASE 0xFEE00000u
#define LAPIC_REG_ID 0x020u
#define LAPIC_REG_EOI 0x0B0u
#define LAPIC_REG_ICR_LOW 0x300u
#define LAPIC_REG_ICR_HIGH 0x310u

/* An unused vector, distinct from the PIC remap range (0x20-0x2F) and
 * the LAPIC timer's own vector -- see lapic_timer.c and irq.c for
 * those. Fixed delivery mode, physical destination, edge-triggered
 * (the defaults for a plain vector write with no extra ICR bits set)
 * is exactly what a "wake up and check now" signal needs: no payload,
 * just "something happened, go look." */
#define CROSS_CORE_SIGNAL_VECTOR 0xF0
#define ICR_DELIVERY_STATUS_PENDING (1u << 12)

static void (*g_handler)(void) = 0;

static inline uint32_t lapic_read(uint32_t reg)
{
    return mmio_read32((const volatile void *)(uintptr_t)(LAPIC_BASE + reg));
}

static inline void lapic_write(uint32_t reg, uint32_t val)
{
    mmio_write32((volatile void *)(uintptr_t)(LAPIC_BASE + reg), val);
}

/* Matches irq.c's own struct interrupt_frame exactly -- this is an ABI
 * contract with the compiler's __attribute__((interrupt)) handling,
 * not a type meant to be shared via a header; irq.c keeps its own copy
 * file-local for the same reason. */
struct interrupt_frame {
    uint64_t ip;
    uint64_t cs;
    uint64_t flags;
    uint64_t sp;
    uint64_t ss;
};

__attribute__((interrupt)) static void isr_cross_core_signal(
    struct interrupt_frame *frame)
{
    (void)frame;
    if (g_handler) {
        g_handler();
    }
    lapic_write(LAPIC_REG_EOI, 0);
}

void cross_core_signal_install_handler(void (*handler)(void))
{
    g_handler = handler;
    irq_install_gate(CROSS_CORE_SIGNAL_VECTOR, (void *)isr_cross_core_signal);
}

uint64_t cross_core_signal_my_target(void)
{
    /* The LAPIC ID register's top byte, same bit layout
     * smp_read_bsp_apic_id already relies on -- valid to read on
     * whichever core executes this, not just the BSP (the existing
     * function's name just predates its use here). */
    return (uint64_t)((lapic_read(LAPIC_REG_ID) >> 24) & 0xFFu);
}

void cross_core_signal_send(uint64_t target)
{
    /* This ICR is shared hardware -- smp.c's send_init_sipi_sipi uses
     * the exact same register for INIT/SIPI. Reusing it while a prior
     * send (from either direction) is still "pending" (bit 12) is a
     * genuine hardware-level race, not just a software one: whichever
     * send is in flight can be corrupted or silently dropped. Real
     * hardware resolves this in a handful of cycles, so a short
     * busy-wait is the conventional, correct way to use the ICR --
     * this is a bounded hardware handshake, not the kind of
     * software-contention spin removed elsewhere in this codebase. */
    while (lapic_read(LAPIC_REG_ICR_LOW) & ICR_DELIVERY_STATUS_PENDING) {
    }
    /* Standard two-write ICR sequence: destination APIC ID into the
     * high dword first, then vector + fixed delivery mode into the low
     * dword, which is what actually triggers the send. */
    lapic_write(LAPIC_REG_ICR_HIGH, (uint32_t)(target & 0xFFu) << 24);
    lapic_write(LAPIC_REG_ICR_LOW, CROSS_CORE_SIGNAL_VECTOR);
}
