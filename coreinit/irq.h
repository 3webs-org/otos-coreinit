#pragma once

#include <stddef.h>
#include <stdint.h>
#include "cap.h"

/* Brings up interrupt handling on THIS core -- the architecture layer
 * configures whatever interrupt controller, vector table, and timer
 * hardware it needs, then enables interrupts. On x86_64: sets up the
 * IDT, remaps the PIC, configures the PIT. On ARM: would configure the
 * GIC and generic timer. The invoke function is saved for dispatching
 * bound IRQ targets via the irq_manager object.
 *
 * All of this lives here, in coreinit, not the kernel -- kernel/ has
 * zero hardware knowledge, exactly per this project's "kernel has no
 * hardware knowledge" principle. This is possible with no kernel
 * mediation at all because coreinit currently runs at the same (only)
 * privilege level as the kernel. The moment real ring separation
 * exists, this becomes exactly the kind of thing that has to move
 * behind a mediated interface instead of executing privileged
 * instructions directly. */
void irq_core_init(int (*kernel_invoke_fn)(capability_t target, uint32_t method_id,
                                           const void *msg, size_t len,
                                           void *response, size_t response_cap,
                                           size_t *out_response_len));

/* The irq_manager object's own handler and schema, created via the factory object
 * exactly like every other object in this system. Method 0 = bind:
 * {irq: u64, target: u64} -> (no response). Binds `target` to be
 * invoked (method 0, message = the tick/IRQ number as a single u64) every
 * time IRQ number `irq` fires. Binding is itself validated the same way
 * any other invocation is -- there is nothing special-cased about it. */
extern schema_table_t g_irq_manager_table;
#define IRQ_SCHEMA_MANAGER 0
void irq_manager_handler(void *data, uint32_t method_id, const void *msg,
                         size_t len, void *response, size_t response_cap,
                         size_t *out_response_len);

/* The legacy PIT tick count, incremented by the architecture's IRQ0/timer
 * handler. Exposed so the LAPIC timer's own calibration (lapic_timer.c)
 * can use the existing, already-running PIT as a one-time time reference
 * without duplicating PIT setup -- after calibration, IRQ0 gets masked
 * and nothing consults this further; the PIT is not this project's
 * ongoing tick source anymore, exactly because it can't be without every
 * core dogpiling on the one physical chip. */
uint64_t irq_get_tick_count(void);

/* Installs a handler at the given vector -- writes to the architecture's
 * interrupt table (IDT on x86_64, vector table on ARM). The table is
 * already loaded by irq_core_init; writes take effect immediately. Used
 * by lapic_timer.c, pagefault.c, and cross_core_signal.c to add their
 * own vectors without needing a second, separate table. */
void irq_install_handler(int vector, void *handler_addr);
