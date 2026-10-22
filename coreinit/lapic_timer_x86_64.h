#pragma once

#include "cap.h"
#include <stdint.h>
#include <stddef.h>

/* x86_64-specific LAPIC timer interface. Internal to the x86_64 implementation;
 * callers outside this file should use the neutral timer.h interface instead.
 *
 * Calibrates and starts THIS core's own Local APIC timer, using the
 * already-running legacy PIT tick purely as a one-time time reference
 * (masking IRQ0 afterward). This is what actually fixes "every core
 * dogpiling on IRQ0": there is no shared hardware here at all after
 * calibration completes. Each core that calls this does its own brief,
 * mutex-protected read of the PIT's tick count (the PIT itself is still
 * shared, legacy, non-SMP-safe hardware -- only the ONE-TIME
 * calibration touches it, not the ongoing tick), then owns its own
 * independent timer from that point on. Must be called after
 * irq_core_init (needs the IDT and the legacy PIT tick already running)
 * and while still holding the hardware mutex used to guard PIC/PIT
 * access.
 */

/* Split calibration and start for a real reason, not just API tidiness:
 * IRQ0 (the legacy PIT's interrupt) is physically wired to the BSP only
 * on standard PC architecture -- an AP's own copy of irq_get_tick_count()
 * never increments no matter how long it waits, so an AP attempting the
 * PIT-based measurement hangs forever. Only the BSP may ever call
 * lapic_timer_calibrate; the resulting count gets published through the
 * discovery record (see smp_layout.h) and every other core calls
 * lapic_timer_start directly with the shared value -- touching only its
 * own LAPIC, never the shared PIC/PIT at all. */
uint32_t lapic_timer_calibrate(uint32_t lapic_base);
void lapic_timer_start(
    uint32_t lapic_base, uint32_t count_per_10ms,
    int (*kernel_invoke_fn)(capability_t target, uint32_t method_id,
                           const void *msg, size_t len, void *response,
                           size_t response_cap, size_t *out_response_len));

/* Combined calibrate+start for the BSP's convenience. */
void lapic_timer_calibrate_and_start(
    uint32_t lapic_base,
    int (*kernel_invoke_fn)(capability_t target, uint32_t method_id,
                           const void *msg, size_t len, void *response,
                           size_t response_cap, size_t *out_response_len));
