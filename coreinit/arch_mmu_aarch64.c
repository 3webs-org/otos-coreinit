/* aarch64 backend for arch_mmu.h. Everything ARM-specific about answering
 * "does THIS core have a working MMU" lives in this one file -- porting to
 * another architecture means writing a new file matching arch_mmu.h's
 * contract, not touching anything that includes that header.
 *
 * The x86_64 backend can answer with a constant: reaching long mode at all
 * REQUIRES paging to be enabled (CR0.PG and CR4.PAE both set), so there is
 * no such thing as an x86_64 core running this image without a functioning
 * MMU already under it. ARM is precisely the architecture that header's
 * comment anticipates: an AArch64 core can legitimately execute with
 * translation disabled -- SCTLR_EL1.M == 0 -- for example immediately after
 * PSCI CPU_ON, before the entry code has enabled the MMU and caches. So the
 * honest implementation reads the hardware state instead of returning 1. */
#include "arch_mmu.h"

/* Bit 0 of SCTLR_EL1 is the M bit: the MMU enable for EL1, the exception
 * level this image runs at. Reading it reports what the core is actually
 * doing right now -- a core still running identity-mapped with translation
 * off returns 0. Cluster join and resource declaration treat that exactly
 * as the header documents: no RESOURCE_SYNC_STREAM or RESOURCE_SYNC_COW
 * resources are declared on a node whose answer is 0, because there is no
 * paging hardware there to split a region and catch the faults. */
int arch_has_mmu(void)
{
    unsigned long sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    return (int)(sctlr & 1ul);
}
