/* aarch64 backend for core_state.h. Everything ARM-specific about storing
 * this core's own core_state pointer lives in this one file -- porting to
 * another architecture means writing a new file matching core_state.h's
 * contract, not touching anything that includes that header. */
#include "core_state.h"

/* TPIDR_EL1 is the architectural EL1 thread/per-CPU pointer register --
 * exactly the purpose-built slot this needs, with no hardware side
 * effect of its own (unlike x86_64's IA32_GS_BASE, this isn't borrowed
 * from an unrelated mechanism). */
void core_state_set(struct core_state *cs)
{
    __asm__ volatile("msr tpidr_el1, %0" : : "r"(cs));
}

struct core_state *core_state_get(void)
{
    struct core_state *cs;
    __asm__ volatile("mrs %0, tpidr_el1" : "=r"(cs));
    return cs;
}
