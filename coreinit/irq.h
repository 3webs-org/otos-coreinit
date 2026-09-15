#pragma once

#include <stddef.h>
#include <stdint.h>
#include "cap.h"

/* Sets up a real IDT, remaps the legacy 8259 PIC (IRQ0-15 -> vectors
 * 0x20-0x2F, away from the CPU-exception range 0-31), configures the
 * PIT (IRQ0) at a demonstrable rate, and enables interrupts (sti).
 *
 * All of this lives here, in coreinit, not the kernel -- kernel/ has
 * zero code anywhere that knows what an interrupt vector, a PIC port,
 * or a PIT frequency is, exactly per this project's "kernel has no
 * hardware knowledge" principle. This is possible with no kernel
 * mediation at all because coreinit currently runs at the same (only)
 * privilege level as the kernel -- lidt/outb are unprivileged from the
 * kernel's perspective simply because nothing enforces otherwise yet.
 * The moment real ring separation exists, this file's contents become
 * exactly the kind of thing that has to move behind a mediated
 * interface instead of executing these instructions directly. */
void irq_init(int (*kernel_invoke_fn)(capability_t target, uint32_t method_id,
                                      const void *msg, size_t len,
                                      void *response, size_t response_cap,
                                      size_t *out_response_len));

/* The irq_manager object's own handler and schema, created via the factory object
 * exactly like every other object in this system. Method 0 = bind:
 * {irq: u64, target: u64} -> (no response). Binds `target` to be
 * invoked (method 0, message = the tick/IRQ number as a single u64) every
 * time IRQ number `irq` fires. Binding is itself validated the same way
 * any other invocation is -- there is nothing special-cased about it. */
extern object_schema_t g_irq_manager_schema;
void irq_manager_handler(void *data, uint32_t method_id, const void *msg,
                         size_t len, void *response, size_t response_cap,
                         size_t *out_response_len);

/* Sets up just the per-core IDT (clear all gates, lidt) -- safe on ANY
 * core, including an AP, since the IDT is per-core copied data, not
 * shared hardware. Does NOT touch the PIC or PIT, does NOT enable
 * interrupts. Use this (not irq_init) on a core that only wants its own
 * LAPIC timer and has no business touching legacy, physically
 * BSP-routed hardware at all -- see lapic_timer.h's own note on why an
 * AP calling the PIT-calibrating path hangs forever. */
void irq_setup_idt_only(void);

/* Remaps the legacy 8259 PIC so IRQ0-7 land on 0x20-0x27 instead of
 * colliding with CPU exception vectors 0x08-0x0F -- see pic_remap's
 * own comment in irq.c. Exposed separately from irq_init/
 * irq_setup_idt_only so it can run standalone, early, before ANYTHING
 * in this boot sequence could possibly enable interrupts (a real,
 * previously-hit bug: run_job_exec_test's very first job_exec_switch
 * resumes into a fake initial frame with IF=1, and until this has run,
 * IRQ0 -- possibly already ticking from firmware/BIOS defaults --
 * fires on vector 8, which the CPU interprets as a double fault, with
 * nowhere valid to vector to at that point in boot). Idempotent --
 * irq_init calls it again later as part of its own full setup, which
 * is harmless. */
void pic_remap(void);

/* The legacy PIT tick count, incremented by isr_irq0. Exposed so the
 * LAPIC timer's own calibration (lapic_timer.c) can use the existing,
 * already-running PIT as a one-time time reference without duplicating
 * PIT setup -- after calibration, IRQ0 gets masked and nothing consults
 * this further; the PIT is not this project's ongoing tick source
 * anymore, exactly because it can't be without every core dogpiling on
 * the one physical chip. */
uint64_t irq_get_tick_count(void);

/* Masks IRQ0 at the PIC -- called once calibration no longer needs it.
 * Leaves the PIT running (harmless, unobserved) but stops it from
 * generating interrupts nothing is listening for anymore. */
void irq_mask_irq0(void);

/* Installs a gate into the SAME IDT irq_init already loaded via lidt --
 * writes to the array take effect immediately, no need to reload the
 * IDT register, since the CPU reads the array directly rather than a
 * cached copy. Used by lapic_timer.c to add its own vector without
 * needing a second, separate IDT. */
void irq_install_gate(int vector, void *handler_addr);
