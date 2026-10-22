#pragma once

/* Idle THIS core until the next interrupt, then return so the caller can
 * re-test whatever it was waiting on. This is the one low-power wait
 * behaviour every node in a cluster is guaranteed to have -- every
 * architecture in scope here has an instruction that stops the core until
 * an interrupt arrives, and every call site below needs exactly that and
 * nothing more. How it is actually done is architecture-specific and
 * lives in exactly one .c file implementing this header, same split as
 * arch_mmu.h and cross_core_signal.h.
 *
 * Deliberately NOT named hlt or wfi: each of those is one architecture's
 * instruction mnemonic, and putting either in the source text of a shared
 * file is precisely the leak this header exists to close. Equally
 * deliberately NOT cpu_relax -- that name means a spin hint that returns
 * immediately (a pause/yield instruction, for a loop the core is about to
 * run again), which is a different thing entirely and would be wrong for
 * every caller here, all of which want the core actually stopped.
 *
 * Safe to call from any context, including a synchronous kernel_invoke
 * handler: it blocks on nothing but the core itself, so it needs no job
 * context to park on, unlike job_exec_wait. That is why the lock retry
 * loops in mem_mgr.c and cluster_scheduler.c -- which run as plain
 * handlers with no job behind them -- can use it. */
void arch_cpu_wait_for_interrupt(void);
