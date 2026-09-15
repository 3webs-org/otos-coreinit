#pragma once

#include "cap.h"

/* Calibrates and starts THIS core's own Local APIC timer, using the
 * already-running legacy PIT tick purely as a one-time time reference
 * (masking IRQ0 afterward -- see irq_mask_irq0). This is what actually
 * fixes "every core dogpiling on IRQ0": there is no shared hardware
 * here at all after calibration completes. Each core that calls this
 * does its own brief, mutex-protected read of the PIT's tick count (the
 * PIT itself is still shared, legacy, non-SMP-safe hardware -- only the
 * ONE-TIME calibration touches it, not the ongoing tick), then owns its
 * own independent timer from that point on. Must be called after
 * irq_init (needs the IDT and the legacy PIT tick already running) and
 * while still holding the hardware mutex used to guard PIC/PIT access.
 */
void lapic_timer_calibrate_and_start(
    uint32_t lapic_base,
    int (*kernel_invoke_fn)(capability_t target, uint32_t method_id,
                           const void *msg, size_t len, void *response,
                           size_t response_cap, size_t *out_response_len));

/* Split from the combined function above for a real reason, not just
 * API tidiness: IRQ0 (the legacy PIT's interrupt) is physically wired to
 * the BSP only on standard PC architecture -- an AP's own copy of
 * irq_get_tick_count() never increments no matter how long it waits,
 * so an AP attempting the PIT-based measurement above hangs forever.
 * Only the BSP may ever call lapic_timer_calibrate; the resulting count
 * gets published through the discovery record (see smp_layout.h) and
 * every other core calls lapic_timer_start directly with the shared
 * value -- touching only its own LAPIC, never the shared PIC/PIT at
 * all, so there's nothing left to hang on and nothing left to
 * re-unmask out from under a core that already finished with it. */
uint32_t lapic_timer_calibrate(uint32_t lapic_base);
void lapic_timer_start(
    uint32_t lapic_base, uint32_t count_per_10ms,
    int (*kernel_invoke_fn)(capability_t target, uint32_t method_id,
                           const void *msg, size_t len, void *response,
                           size_t response_cap, size_t *out_response_len));

/* Binds a capability to be invoked (method 0, message = tick count as a
 * single u64) on every timer tick. Deliberately an ordinary function
 * call, not a capability object like irq_manager's bind -- this is
 * purely same-core bookkeeping (nothing outside this core could
 * meaningfully bind ITS OWN timer target), so the extra indirection of
 * a schema-validated object round-trip wouldn't buy anything here. */
void lapic_timer_bind(capability_t target);

uint64_t lapic_timer_get_ticks(void);

/* Sends End-Of-Interrupt to the LAPIC -- exposed so job_exec's own
 * hand-written preemption ISR (job_exec_timer_isr.S) can acknowledge
 * the interrupt itself, since it bypasses isr_lapic_timer entirely
 * while a preemption test is installed (see
 * lapic_timer_install_isr). */
void lapic_timer_eoi(void);

/* Installs a DIFFERENT ISR at the LAPIC timer's own vector, replacing
 * whatever is currently there (isr_lapic_timer, normally). Exists so
 * job_exec's preemption test can temporarily take over the timer
 * without touching lapic_timer.c's own internals, and hand it back
 * (call this again with isr_lapic_timer's own address, which stays
 * private to lapic_timer.c -- see lapic_timer_restore_normal_isr)
 * once done. */
void lapic_timer_install_isr(void *isr);
void lapic_timer_restore_normal_isr(void);
