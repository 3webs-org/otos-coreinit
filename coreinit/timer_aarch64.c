/* ARM Generic Timer implementation (aarch64).
 *
 * Uses the physical timer (CNTP_*) driven by CNTPCT_EL0. The timer runs at
 * a fixed frequency (typically 62.5 MHz on QEMU virt, but read from
 * CNTFRQ_EL0 or topology discovery). We program CNTP_TVAL_EL0 with a
 * relative tick count; when it counts down to zero, the timer fires an
 * interrupt. On each interrupt, we re-write TVAL to re-arm for the next
 * tick -- this is simpler than using CVAL (compare value) and avoids drift
 * from interrupt latency since TVAL is relative to "now".
 *
 * The interrupt itself is routed through the GIC (irq_aarch64.c handles
 * ack/EOI); this file just manages the timer hardware and the ISR that
 * increments the tick count and invokes the bound capability.
 */

#include "timer.h"
#include "cap.h"
#include "capnp_build.h"
#include "irq.h"
#include "serial.h"
#include <stdint.h>
#include <stddef.h>

/* ARM Generic Timer system registers (accessed via MRS/MSR). */
#define TIMER_VECTOR 30  /* PPI 14 = GIC INTID 30 (16 + 14) */

static uint64_t g_timer_freq_hz = 0;
static uint64_t g_ticks = 0;
static capability_t g_timer_target;
static int (*g_kernel_invoke)(capability_t, uint32_t, const void *, size_t,
                              void *, size_t, size_t *);
static void *g_normal_isr = NULL;

/* Read CNTFRQ_EL0: timer frequency in Hz. */
static inline uint64_t read_cntfrq(void)
{
    uint64_t val;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(val));
    return val;
}

/* Read CNTPCT_EL0: physical counter (free-running, monotonic). */
static inline uint64_t read_cntpct(void)
{
    uint64_t val;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(val));
    return val;
}

/* Write CNTP_TVAL_EL0: timer value (relative, counts down). Writing N
 * schedules an interrupt after N ticks. Re-writing it re-arms the timer. */
static inline void write_cntp_tval(uint32_t val)
{
    __asm__ volatile("msr cntp_tval_el0, %0" : : "r"((uint64_t)val));
}

/* Read CNTP_CTL_EL0: timer control register. */
static inline uint64_t read_cntp_ctl(void)
{
    uint64_t val;
    __asm__ volatile("mrs %0, cntp_ctl_el0" : "=r"(val));
    return val;
}

/* Write CNTP_CTL_EL0: timer control register.
 * Bit 0: ENABLE (1 = timer enabled)
 * Bit 1: IMASK (0 = interrupt unmasked, 1 = masked)
 * Bit 2: ISTATUS (read-only, 1 = condition met) */
static inline void write_cntp_ctl(uint64_t val)
{
    __asm__ volatile("msr cntp_ctl_el0, %0" : : "r"(val));
}

/* Default timer ISR: increment tick count, invoke bound capability, re-arm.
 * The GIC (irq_aarch64.c) handles interrupt ack/EOI; we just manage the
 * timer state here. This ISR is NOT marked with __attribute__((interrupt))
 * because the vector table (not yet written) will handle the low-level
 * entry; this is called FROM that entry path. */
static void isr_arm_timer(void)
{
    g_ticks++;
    if (g_timer_target.handle != 0) {
        uint64_t word = g_ticks;
        uint8_t msg[24];
        size_t msg_len = capnp_build_flat(msg, sizeof(msg), &word, 1);
        g_kernel_invoke(g_timer_target, 0, msg, msg_len, NULL, 0, NULL);
    }
    
    /* Re-arm the timer for the next tick. TVAL is relative to "now", so
     * writing the same interval again schedules the next interrupt. This
     * approach (TVAL) is simpler than CVAL (absolute compare value) and
     * naturally compensates for interrupt latency -- each tick is relative
     * to when we re-arm, not to a fixed schedule that could drift. */
    uint32_t interval = (uint32_t)(g_timer_freq_hz / 100);  /* ~100Hz */
    write_cntp_tval(interval);
}

/* --- Architecture-neutral timer.h interface implementation --- */

uint64_t timer_core_init(
    uint64_t base_or_calibration,
    uint64_t calibration_hint,
    int (*kernel_invoke_fn)(capability_t, uint32_t, const void *, size_t,
                           void *, size_t, size_t *))
{
    (void)calibration_hint;  /* Unused on aarch64 (no BSP/AP split). */
    
    /* base_or_calibration on aarch64: timer frequency in Hz from topology
     * discovery (arch_opaque[2]), or 0 to read CNTFRQ_EL0 directly. */
    if (base_or_calibration == 0) {
        g_timer_freq_hz = read_cntfrq();
        kprintf("[coreinit]   arm_timer: frequency read from CNTFRQ_EL0: "
                "%llu Hz\n", (unsigned long long)g_timer_freq_hz);
    } else {
        g_timer_freq_hz = base_or_calibration;
        kprintf("[coreinit]   arm_timer: frequency from topology: %llu Hz\n",
                (unsigned long long)g_timer_freq_hz);
    }
    
    g_kernel_invoke = kernel_invoke_fn;
    g_timer_target.handle = 0;
    g_ticks = 0;
    
    /* Install our ISR at the timer vector. The actual exception vector table
     * (assembly, not yet written) will dispatch to this function. We record
     * the ISR address so timer_install_isr / timer_restore_normal_isr can
     * swap it out for job_exec's preemption test. */
    g_normal_isr = (void *)isr_arm_timer;
    irq_install_handler(TIMER_VECTOR, g_normal_isr);
    
    /* Program the timer: enable it, unmask interrupts, set the interval.
     * TVAL is relative, so writing it now schedules the first interrupt. */
    uint32_t interval = (uint32_t)(g_timer_freq_hz / 100);  /* ~100Hz */
    write_cntp_tval(interval);
    write_cntp_ctl(0x1);  /* ENABLE=1, IMASK=0 (unmasked) */
    
    kprintf("[coreinit]   arm_timer: running at ~100Hz, vector %u "
            "(interval: %u ticks)\n", TIMER_VECTOR, interval);
    
    return 0;  /* aarch64 has no calibration to return */
}

void timer_bind(capability_t target)
{
    g_timer_target = target;
}

uint64_t timer_get_ticks(void)
{
    return g_ticks;
}

void timer_eoi(void)
{
    /* No-op on aarch64: the GIC (irq_aarch64.c) handles EOI via ICC_EOIR1_EL1
     * in its own interrupt dispatch path. The timer itself has no separate
     * EOI register -- re-arming TVAL (done in the ISR) clears ISTATUS. */
}

void timer_install_isr(void *isr)
{
    irq_install_handler(TIMER_VECTOR, isr);
}

void timer_restore_normal_isr(void)
{
    irq_install_handler(TIMER_VECTOR, g_normal_isr);
}
