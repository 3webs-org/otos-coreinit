/* Per-core EL1 identity map for QEMU virt's 512 MiB ARM reference machine.
 * The bootloader (and PSCI on every AP) enters saski/coreinit with the MMU
 * off. A 4 KiB L1 table maps device MMIO below RAM as Device-nGnRE and the
 * 0x40000000..0x80000000 window as inner-shareable normal WB memory.
 * The running kernel, init, AP regions, mailbox, EFI tables, PL011 and GIC
 * retain identical virtual and physical addresses across the switch.
 *
 * The root lives in struct core_state. That is required when APs execute the
 * same Mach-O image: an image-global root lets one AP erase or replace the
 * BSP's live translations. The complete 32-bit peripheral aperture is
 * mapped; blocks without ordinary RAM use Device-nGnRE attributes. */
#include "arch_mmu.h"
#include "core_state.h"
#include <stdint.h>

#define ARM_RAM_BASE 0x40000000ull
#define DESC_BLOCK 0x1ull
#define DESC_AF (1ull << 10)
#define DESC_UXN (1ull << 54)
#define DESC_PXN (1ull << 53)
#define DESC_NORMAL (1ull << 2)
#define DESC_INNER_SHAREABLE (3ull << 8)

int arch_has_mmu(void)
{
    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    return (sctlr & 1ull) != 0;
}

uint64_t arch_mmu_root_phys(void)
{
    struct core_state *cs = core_state_get();
    return cs && cs->mmu_owned_map ? (uint64_t)(uintptr_t)cs->mmu_l1 : 0;
}

int arch_mmu_enable_identity_map(uint64_t kernel_invoke_addr)
{
    uint64_t current_el, sctlr;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(current_el));
    if (current_el != (1ull << 2)) {
        return 0; /* EL1 owns TTBR0_EL1; never write it from another EL. */
    }
    struct core_state *cs = core_state_get();
    if (!cs) {
        return 0;
    }
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    if (sctlr & 1ull) {
        return 1; /* Preserve an already-active platform map. */
    }

    for (unsigned i = 0; i < 512; i++) {
        cs->mmu_l1[i] = 0;
    }
    cs->mmu_l1[0] = DESC_BLOCK | DESC_AF | DESC_UXN | DESC_PXN;
    cs->mmu_l1[1] = ARM_RAM_BASE | DESC_BLOCK | DESC_AF | DESC_NORMAL |
                    DESC_INNER_SHAREABLE | DESC_UXN;
    cs->mmu_l1[2] = 0x80000000ull | DESC_BLOCK | DESC_AF | DESC_UXN |
                    DESC_PXN;
    cs->mmu_l1[3] = 0xC0000000ull | DESC_BLOCK | DESC_AF | DESC_UXN |
                    DESC_PXN;

    /* The callable Saski image is ordinary executable RAM. On virt it
     * shares block 1 with coreinit; RAM-at-zero boards place it in block 0.
     * Mark the block containing the handed-over kernel entry accordingly so
     * the first kernel_invoke callback cannot execute from Device/PXN space. */
    unsigned kernel_block = (unsigned)(kernel_invoke_addr >> 30);
    if (kernel_block < 4) {
        cs->mmu_l1[kernel_block] = ((uint64_t)kernel_block << 30) |
            DESC_BLOCK | DESC_AF | DESC_NORMAL | DESC_INNER_SHAREABLE |
            DESC_UXN;
    }

    /* MAIR index 0 = Device-nGnRE (0x04), index 1 = Normal WBWA (0xff).
     * 39-bit TTBR0 VA, 4 KiB granule, inner-shareable WB walks; TTBR1
     * walks disabled. Every mapped physical address fits in 32 bits. */
    const uint64_t mair = 0xff04ull;
    const uint64_t tcr = 25ull | (16ull << 16) | (1ull << 23) |
                         (1ull << 8) | (1ull << 10) | (3ull << 12);
    const uint64_t ttbr = (uint64_t)(uintptr_t)cs->mmu_l1;
    __asm__ volatile(
        "msr daifset, #2\n\t"
        "dsb sy\n\t"
        "msr mair_el1, %0\n\t"
        "msr tcr_el1, %1\n\t"
        "msr ttbr0_el1, %2\n\t"
        "isb\n\t"
        "tlbi vmalle1\n\t"
        "dsb sy\n\t"
        "isb"
        : : "r"(mair), "r"(tcr), "r"(ttbr) : "memory");
    sctlr |= (1ull << 0) | (1ull << 2) | (1ull << 12); /* M,C,I */
    __asm__ volatile("msr sctlr_el1, %0\n\tisb" : : "r"(sctlr) : "memory");
    cs->mmu_owned_map = 1;
    return arch_has_mmu();
}
