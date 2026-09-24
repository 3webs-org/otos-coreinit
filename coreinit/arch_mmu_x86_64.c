#include "arch_mmu.h"
#include <stdint.h>

/* On x86_64, this is trivially always true, not a placeholder: reaching
 * long mode at all -- which every core running this coreinit image
 * already has, by construction, before coreinit_main ever runs --
 * REQUIRES paging to be enabled (CR0.PG and CR4.PAE both set; see
 * kernel/boot.S's own transition and kernel.c's build_ap_page_tables
 * for the AP path). There is no such thing as an x86_64 core in long
 * mode without a functioning MMU already active under it. A different
 * architecture's implementation of this same header is where a real
 * "no, this core has none" answer could actually occur -- e.g. a
 * simple accelerator or microcontroller core joining the cluster with
 * no paging hardware at all. */
int arch_has_mmu(void)
{
    return 1;
}

#include "../include/smp_layout.h"

#define PTE_PRESENT 0x1ull
#define PTE_WRITABLE 0x2ull
#define PTE_HUGE 0x80ull

uint64_t arch_build_page_tables(uint64_t region_base)
{
    uint64_t pml4_phys = region_base + CORE_PGTABLE_OFFSET + CORE_PGTABLE_PML4_OFF;
    uint64_t pdpt_phys = region_base + CORE_PGTABLE_OFFSET + CORE_PGTABLE_PDPT_OFF;
    uint64_t pd_phys = region_base + CORE_PGTABLE_OFFSET + CORE_PGTABLE_PD_OFF;
    uint64_t pd_hi_phys = region_base + CORE_PGTABLE_OFFSET + CORE_PGTABLE_PD_HI_OFF;

    uint64_t *pml4 = (uint64_t *)(uintptr_t)pml4_phys;
    uint64_t *pdpt = (uint64_t *)(uintptr_t)pdpt_phys;
    uint64_t *pd = (uint64_t *)(uintptr_t)pd_phys;
    uint64_t *pd_hi = (uint64_t *)(uintptr_t)pd_hi_phys;

    for (int i = 0; i < 512; i++) {
        pml4[i] = 0;
        pdpt[i] = 0;
    }

    /* Identity-mapped GB0 and GB3 in 2 MiB pages: GB0 covers everything
     * this project actually places (kernel, init, every core region), GB3
     * covers the LAPIC/IOAPIC MMIO window at 0xFEC00000-0xFEE00000. */
    pml4[0] = pdpt_phys | PTE_PRESENT | PTE_WRITABLE;
    pdpt[0] = pd_phys | PTE_PRESENT | PTE_WRITABLE;
    pdpt[3] = pd_hi_phys | PTE_PRESENT | PTE_WRITABLE;

    for (int i = 0; i < 512; i++) {
        pd[i] = ((uint64_t)i * 0x200000ull) | PTE_PRESENT | PTE_WRITABLE |
               PTE_HUGE;
        pd_hi[i] = (0xC0000000ull + (uint64_t)i * 0x200000ull) |
                  PTE_PRESENT | PTE_WRITABLE | PTE_HUGE;
    }

    return pml4_phys;
}
