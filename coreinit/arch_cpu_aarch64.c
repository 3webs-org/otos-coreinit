/* aarch64 backend for arch_cpu.h. Everything ARM-specific about idling a
 * core until the next interrupt lives in this one file -- porting to
 * another architecture means writing a new file matching arch_cpu.h's
 * contract, not touching anything that includes that header. */
#include "arch_cpu.h"

/* WFI is the architectural ARMv8 wait-for-interrupt: the core enters a
 * low-power state and resumes once an unmasked asynchronous exception
 * becomes pending, which is the same guarantee arch_cpu.h makes on every
 * backend. The "memory" clobber keeps the compiler from hoisting the flag
 * the caller is re-testing across the wait. */
void arch_cpu_wait_for_interrupt(void)
{
    __asm__ volatile("wfi" : : : "memory");
}
