/* aarch64 backend for pagefault.h.
 *
 * ARMv8 fault-driven STREAM/COW is a future unit; an honest stub backend
 * now beats a fake implementation. Every entry point pagefault.h
 * declares is answered here so the neutral API links on a node that has
 * no page-fault mechanics: this node reports pagefault_supported() == 0,
 * every caller gates on that and never reaches these bodies, exactly as
 * arch_mmu.h anticipates for nodes without an MMU. */
#include "pagefault.h"

/* Reports 0 on this architecture: no fault trap is installed, so no
 * page here can be marked absent or copy-on-write and resolved on
 * access. */
int pagefault_supported(void)
{
    return 0;
}

/* Unsupported stubs. Each exists only so the neutral API links; callers
 * gate on pagefault_supported() and never reach here. */

void pagefault_init(uint64_t pml4_phys)
{
    (void)pml4_phys;
}

int pagefault_split_region(uint64_t pml4_phys, uint64_t region_base)
{
    (void)pml4_phys;
    (void)region_base;
    return 0;
}

int pagefault_mark_absent(uint64_t pml4_phys, uint64_t page_addr)
{
    (void)pml4_phys;
    (void)page_addr;
    return 0;
}

void pagefault_register_streamed_page(uint64_t page_addr)
{
    (void)page_addr;
}

int pagefault_mark_cow(uint64_t pml4_phys, uint64_t page_addr)
{
    (void)pml4_phys;
    (void)page_addr;
    return 0;
}

uint64_t pagefault_get_cow_copy_count(void)
{
    return 0;
}

uint64_t pagefault_get_count(void)
{
    return 0;
}

uint64_t pagefault_debug_read_pte(uint64_t pml4_phys, uint64_t addr)
{
    (void)pml4_phys;
    (void)addr;
    return 0;
}
