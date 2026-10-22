/* x86_64 backend for arch_cpu.h. Everything x86-specific about idling a
 * core until the next interrupt lives in this one file -- porting to
 * another architecture means writing a new file matching arch_cpu.h's
 * contract, not touching anything that includes that header. */
#include "arch_cpu.h"

/* HLT stops the core until the next interrupt (NMI, maskable interrupt,
 * or SMI) without consuming execution bandwidth while it waits -- exactly
 * the portable behaviour arch_cpu.h promises. The "memory" clobber keeps
 * the compiler from hoisting the flag the caller is re-testing across the
 * wait, which is the whole point of the retry loops at every call site. */
void arch_cpu_wait_for_interrupt(void)
{
    __asm__ volatile("hlt" : : : "memory");
}
