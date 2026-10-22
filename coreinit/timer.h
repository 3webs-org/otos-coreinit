#pragma once

#include "cap.h"
#include <stddef.h>
#include <stdint.h>

/* Architecture-neutral timer interface. Each architecture implements this
 * with its own hardware timer (x86_64: Local APIC timer calibrated against
 * the PIT; aarch64: ARM Generic Timer driven by CNTPCT_EL0).
 *
 * The timer runs at ~100Hz and can invoke a bound capability on each tick.
 * Initialization is split (BSP calibrates, APs reuse the result) on x86_64
 * where the legacy PIT is wired to the BSP only; aarch64 has no such
 * constraint but accepts the same interface for consistency.
 */

/* Initialize and start the timer on this core.
 *
 * base_or_calibration: architecture-specific base address or calibration.
 *   x86_64: LAPIC MMIO base address (from topology discovery).
 *   aarch64: timer frequency in Hz (from topology discovery), or 0 to read
 *            CNTFRQ_EL0 directly.
 *
 * calibration_hint: architecture-specific calibration data.
 *   x86_64: LAPIC counts per 10ms (from prior calibration), or 0 to
 *           calibrate now (BSP only; requires irq_core_init first).
 *   aarch64: unused, pass 0.
 *
 * kernel_invoke_fn: the kernel_invoke function pointer, needed to invoke
 *                   the bound capability on each tick.
 *
 * Returns the calibration value (x86_64: counts per 10ms; aarch64: 0).
 * This allows the BSP to pass the calibration to APs via the discovery record.
 */
uint64_t timer_core_init(
    uint64_t base_or_calibration,
    uint64_t calibration_hint,
    int (*kernel_invoke_fn)(capability_t target, uint32_t method_id,
                           const void *msg, size_t len, void *response,
                           size_t response_cap, size_t *out_response_len));

/* Bind a capability to be invoked on every timer tick. The capability is
 * invoked with method 0 and a single u64 message containing the tick count.
 * Pass a zero-handle capability to unbind.
 */
void timer_bind(capability_t target);

/* Read the current tick count (monotonic, starts at 0 on timer init). */
uint64_t timer_get_ticks(void);

/* Acknowledge the timer interrupt. x86_64 requires an explicit LAPIC EOI;
 * aarch64 is a no-op (GIC EOI is handled by irq_aarch64.c). Exposed so
 * job_exec's preemption test ISR can acknowledge the interrupt itself.
 */
void timer_eoi(void);

/* Install a different ISR at the timer's vector, replacing the default.
 * Used by job_exec's preemption test to temporarily take over the timer.
 * The ISR must be a function pointer suitable for the architecture's
 * interrupt calling convention.
 */
void timer_install_isr(void *isr);

/* Restore the default timer ISR (the one installed by timer_core_init).
 * Used by job_exec's preemption test to hand the timer back after testing.
 */
void timer_restore_normal_isr(void);
