#pragma once

#include <stdint.h>

/* Cross-node preemption needs a way to make a specific OTHER core take
 * an interrupt right now, so it can immediately reevaluate whether to
 * preempt whatever it's currently running -- polling a mailbox on the
 * receiving core's own idle-loop checks (the existing mechanism) isn't
 * enough for that, since a busy core might not check for a while.
 *
 * That "make a specific other core interrupt itself" operation is
 * inherently a hardware facility, and a genuinely different one per
 * architecture (an IPI via the Local APIC on x86_64; a Software
 * Generated Interrupt via the GIC on ARM; whatever the equivalent is
 * elsewhere) -- so this header is the entire portable surface, with
 * every architecture-specific detail (what "target" means, which
 * register gets written, which vector fires) confined to exactly one
 * .c file implementing it. Nothing above this header -- cluster
 * scheduling, job_exec, anywhere cross-node preemption gets decided --
 * should ever need to know which architecture it's running on. */

/* Causes the core identified by target to take an interrupt right now.
 * target is architecture-opaque from every caller's perspective: it's
 * whatever cross_core_signal_my_target() produced on the RECEIVING
 * core when it joined the cluster (see cluster_scheduler.c's
 * handle_join_node, which records it verbatim in node_data_t without
 * ever interpreting it) and is being handed back here unchanged. */
void cross_core_signal_send(uint64_t target);

/* Registers the callback invoked on THIS core when a signal sent via
 * cross_core_signal_send(this core's own target) arrives. Must be
 * called once, early, on every core that wants to be validly
 * signalable -- this is its own dedicated mechanism, not a repurposing
 * of any existing IRQ/timer vector. */
void cross_core_signal_install_handler(void (*handler)(void));

/* This core's own architecture-opaque target value -- what a caller
 * elsewhere should pass to cross_core_signal_send to reach THIS core.
 * Computed fresh by the architecture layer (on x86_64: this core's own
 * Local APIC ID); callers above this header just carry the returned
 * value around without inspecting it. Intended to be read once, at
 * cluster-join time, and handed to JOIN_NODE alongside node_id/arch/
 * region_base -- see coreinit.c's own join call and
 * cluster_scheduler.c's handle_join_node. */
uint64_t cross_core_signal_my_target(void);
