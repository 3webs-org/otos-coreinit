#include "pagefault.h"
#include "irq.h"
#include "serial.h"
#include "core_state.h"

#define MAX_STREAMED_PAGES 16
#define MAX_COW_PAGES 16
#define COW_POOL_SIZE 4

#define PAGEFAULT_VECTOR 0x0E /* CPU exception 14, #PF -- fixed by the
                               * x86-64 architecture itself, not a
                               * choice this project makes the way the
                               * LAPIC timer's vector 0x22 was */

/* g_split_pt/g_split_region_base/g_have_split, the streamed/COW tracking
 * arrays, and the COW pool itself all now live in struct core_state --
 * all genuinely per-core (each core's own page tables and own fault
 * history), see that header's own note. g_pagefault_pml4_phys is GONE
 * entirely, not migrated: it was always set to the same value
 * job_memory.c already records in core_state->my_pml4_phys (confirmed by
 * tracing the one caller, job_memory_acquire_resource), so isr_page_fault
 * reads that field directly instead of keeping a second copy that could
 * drift out of sync with the first.
 *
 * g_ring3_hook stays a plain global, deliberately -- irq_x86_64.c's own
 * comment on ring-3 setup already establishes that only a trusted
 * wrapper object installs this, on the BSP, same BSP-singleton category
 * as that file's g_irq_targets. */
static pagefault_ring3_hook_t g_ring3_hook;

void pagefault_x86_64_set_ring3_hook(pagefault_ring3_hook_t hook)
{
    __atomic_store_n(&g_ring3_hook, hook, __ATOMIC_RELEASE);
}

struct interrupt_frame {
    uint64_t ip;
    uint64_t cs;
    uint64_t flags;
    uint64_t sp;
    uint64_t ss;
};

static inline void invlpg(uint64_t addr)
{
    __asm__ volatile("invlpg (%0)" : : "r"(addr) : "memory");
}

static uint64_t *walk_to_pd_entry(uint64_t pml4_phys, uint64_t vaddr)
{
    uint64_t pml4_idx = (vaddr >> 39) & 0x1FF;
    uint64_t pdpt_idx = (vaddr >> 30) & 0x1FF;
    uint64_t pd_idx = (vaddr >> 21) & 0x1FF;

    uint64_t *pml4 = (uint64_t *)(uintptr_t)pml4_phys;
    if (!(pml4[pml4_idx] & PAGE_PRESENT)) {
        return NULL;
    }
    uint64_t *pdpt = (uint64_t *)(uintptr_t)(pml4[pml4_idx] & PAGE_ADDR_MASK);
    if (!(pdpt[pdpt_idx] & PAGE_PRESENT)) {
        return NULL;
    }
    uint64_t *pd = (uint64_t *)(uintptr_t)(pdpt[pdpt_idx] & PAGE_ADDR_MASK);
    return &pd[pd_idx];
}

static uint64_t *walk_to_pt_entry(uint64_t pml4_phys, uint64_t vaddr)
{
    uint64_t *pd_entry = walk_to_pd_entry(pml4_phys, vaddr);
    if (!pd_entry || (*pd_entry & PAGE_HUGE) || !(*pd_entry & PAGE_PRESENT)) {
        return NULL; /* not present, still huge, or genuinely absent above */
    }
    uint64_t *pt = (uint64_t *)(uintptr_t)(*pd_entry & PAGE_ADDR_MASK);
    uint64_t pt_idx = (vaddr >> 12) & 0x1FF;
    return &pt[pt_idx];
}

int pagefault_split_region(uint64_t pml4_phys, uint64_t region_base)
{
    struct core_state *cs = core_state_get();
    if (cs == NULL) {
        return 0;
    }
    uint64_t *pd_entry = walk_to_pd_entry(pml4_phys, region_base);
    if (!pd_entry || !(*pd_entry & PAGE_PRESENT) || !(*pd_entry & PAGE_HUGE)) {
        kprintf("[coreinit]   pagefault: split failed, region 0x%X isn't "
                "mapped as a huge page\n",
                (unsigned)region_base);
        return 0;
    }

    uint64_t huge_base = *pd_entry & ~0x1FFFFFull; /* 2MB-aligned */
    for (int i = 0; i < 512; i++) {
        cs->pf_split_pt[i] = (huge_base + (uint64_t)i * 4096) | PAGE_PRESENT |
                            PAGE_WRITABLE;
    }

    *pd_entry = (uint64_t)(uintptr_t)cs->pf_split_pt | PAGE_PRESENT | PAGE_WRITABLE;
    /* Invalidate EVERY 4KB address in the region, not just region_base
     * -- a huge-page TLB entry covers the whole 2MB range as a single
     * cached translation, and invlpg on one address within that range
     * is architecturally supposed to invalidate it for the whole
     * range, but this is cheap insurance against any TLB implementation
     * detail (including QEMU TCG's own) that caches per-4KB-address
     * instead. */
    for (uint64_t off = 0; off < 0x200000ull; off += 0x1000ull) {
        invlpg(region_base + off);
    }

    cs->pf_split_region_base = huge_base;
    cs->pf_have_split = 1;
    kprintf("[coreinit]   pagefault: split 2MB region 0x%X into a real "
            "4KB-granular mapping, identical access preserved\n",
            (unsigned)huge_base);
    return 1;
}

void pagefault_register_streamed_page(uint64_t page_addr)
{
    struct core_state *cs = core_state_get();
    if (cs == NULL) {
        return;
    }
    if (cs->pf_streamed_count < MAX_STREAMED_PAGES) {
        cs->pf_streamed_pages[cs->pf_streamed_count++] = page_addr;
    }
}

static int is_registered_streamed(struct core_state *cs, uint64_t addr)
{
    for (size_t i = 0; i < cs->pf_streamed_count; i++) {
        if (cs->pf_streamed_pages[i] == addr) {
            return 1;
        }
    }
    return 0;
}

static int is_registered_cow(struct core_state *cs, uint64_t addr)
{
    for (size_t i = 0; i < cs->pf_cow_count; i++) {
        if (cs->pf_cow_pages[i] == addr) {
            return 1;
        }
    }
    return 0;
}

int pagefault_mark_absent(uint64_t pml4_phys, uint64_t page_addr)
{
    struct core_state *cs = core_state_get();
    if (cs == NULL) {
        return 0;
    }
    uint64_t *pt_entry = walk_to_pt_entry(pml4_phys, page_addr);
    if (!pt_entry) {
        kprintf("[coreinit]   pagefault: mark_absent failed, 0x%X is not "
                "within a split region\n",
                (unsigned)page_addr);
        return 0;
    }
    *pt_entry &= ~PAGE_PRESENT; /* address bits and the writable bit stay
                                 * intact -- resolving just needs to set
                                 * PRESENT back */
    invlpg(page_addr);
    pagefault_register_streamed_page(page_addr);
    kprintf("[coreinit]   pagefault: page 0x%X marked absent (streamed) "
            "-- next access will fault\n",
            (unsigned)page_addr);
    return 1;
}

int pagefault_mark_cow(uint64_t pml4_phys, uint64_t page_addr)
{
    struct core_state *cs = core_state_get();
    if (cs == NULL) {
        return 0;
    }
    uint64_t *pt_entry = walk_to_pt_entry(pml4_phys, page_addr);
    if (!pt_entry || !(*pt_entry & PAGE_PRESENT)) {
        kprintf("[coreinit]   pagefault: mark_cow failed, 0x%X isn't a "
                "present page within a split region\n",
                (unsigned)page_addr);
        return 0;
    }
    *pt_entry &= ~PAGE_WRITABLE; /* stays present -- reads are free,
                                  * forever; only a write faults */
    invlpg(page_addr);
    if (cs->pf_cow_count < MAX_COW_PAGES) {
        cs->pf_cow_pages[cs->pf_cow_count++] = page_addr;
    }
    kprintf("[coreinit]   pagefault: page 0x%X marked copy-on-write -- "
            "reads free, first write will fault and copy\n",
            (unsigned)page_addr);
    return 1;
}

static void resolve_streamed(struct core_state *cs, uint64_t fault_addr,
                             uint64_t pml4_phys)
{
    (void)cs;
    /* Single-box model: the data is already physically here (every core
     * shares the same physical memory), so "fetching" it is a no-op --
     * there's only a page table entry missing, not the data itself. A
     * cross-machine implementation would fetch the current contents
     * over the network into a freshly claimed local physical frame
     * HERE, before the present bit goes up. */
    uint64_t *pt_entry = walk_to_pt_entry(pml4_phys, fault_addr);
    if (pt_entry) {
        *pt_entry |= PAGE_PRESENT;
        invlpg(fault_addr);
    }
}

static void resolve_cow_write(struct core_state *cs, uint64_t fault_addr,
                              uint64_t pml4_phys)
{
    uint64_t *pt_entry = walk_to_pt_entry(pml4_phys, fault_addr);
    if (!pt_entry) {
        return;
    }

    if (cs->pf_cow_pool_used >= COW_POOL_SIZE) {
        kprintf("[coreinit]   pagefault: COW pool exhausted, cannot "
                "resolve write to 0x%X\n",
                (unsigned)fault_addr);
        return;
    }

    const uint8_t *shared_source =
        (const uint8_t *)(uintptr_t)(*pt_entry & PAGE_ADDR_MASK);
    uint8_t *private_copy = cs->pf_cow_pool[cs->pf_cow_pool_used++];
    for (int i = 0; i < 4096; i++) {
        private_copy[i] = shared_source[i];
    }

    *pt_entry = ((uint64_t)(uintptr_t)private_copy & PAGE_ADDR_MASK) |
               PAGE_PRESENT | PAGE_WRITABLE;
    invlpg(fault_addr);
    cs->pf_cow_copy_count++;
    kprintf("[coreinit]   pagefault: COW copy made for 0x%X -- this "
            "context now has its own private page, the shared original "
            "is untouched\n",
            (unsigned)fault_addr);
}

__attribute__((interrupt)) static void isr_page_fault(struct interrupt_frame *frame,
                                                       uint64_t error_code)
{
    (void)frame;
    uint64_t fault_addr;
    __asm__ volatile("mov %%cr2, %0" : "=r"(fault_addr));

    pagefault_ring3_hook_t hook = __atomic_load_n(&g_ring3_hook, __ATOMIC_ACQUIRE);
    if ((error_code & 4u) && hook &&
        hook((uint64_t *)frame, error_code, fault_addr)) {
        return; /* Wrapper handles the CPL3 fault and resumes/terminates. */
    }
    uint64_t page_addr = fault_addr & ~0xFFFull;
    int was_present = (error_code & 0x1) != 0;
    int was_write = (error_code & 0x2) != 0;

    /* No core_state means none of the per-core fault-tracking below can
     * run at all -- fall straight through to the existing "unhandled"
     * halt rather than invent a new failure path for it. */
    struct core_state *cs = core_state_get();
    if (cs != NULL) {
        cs->pf_fault_count++;

        if (!was_present && is_registered_streamed(cs, page_addr)) {
            resolve_streamed(cs, page_addr, cs->my_pml4_phys);
            return;
        }
        if (was_present && was_write && is_registered_cow(cs, page_addr)) {
            resolve_cow_write(cs, page_addr, cs->my_pml4_phys);
            return;
        }
    }

    kprintf("[coreinit] UNHANDLED PAGE FAULT at 0x%X, error_code=0x%X "
            "(present=%d write=%d) -- halting\n",
            (unsigned)fault_addr, (unsigned)error_code, was_present,
            was_write);
    for (;;) {
        __asm__ volatile("hlt");
    }
}

/* This backend installs the real #PF gate and resolves faults, so the
 * mechanics genuinely exist on the platforms it targets. */
int pagefault_supported(void)
{
    return 1;
}

void pagefault_init(uint64_t pml4_phys)
{
    (void)pml4_phys; /* see this file's own top note -- isr_page_fault
                      * reads core_state->my_pml4_phys directly instead */
    irq_install_handler(PAGEFAULT_VECTOR, (void *)isr_page_fault);

    /* CR0.WP defaults to clear after reset, meaning supervisor-mode
     * code (all of this project, so far -- there is no ring 3 yet)
     * simply ignores the read-only bit in page tables entirely: "when
     * clear, allows supervisor-level procedures to write into
     * read-only pages" (Intel SDM Vol 3A). Without this, COW's whole
     * premise -- that a write to a read-only page faults at all --
     * would silently not hold for any of this project's own code. This
     * is the exact bit the SDM itself documents as existing to make
     * COW possible. */
    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= (1ull << 16); /* WP */
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));

    kprintf("[coreinit]   pagefault: #PF handler installed, CR0.WP "
            "enabled (required for COW to fault at all from ring 0)\n");
}

uint64_t pagefault_get_count(void)
{
    struct core_state *cs = core_state_get();
    return cs != NULL ? cs->pf_fault_count : 0;
}

uint64_t pagefault_get_cow_copy_count(void)
{
    struct core_state *cs = core_state_get();
    return cs != NULL ? cs->pf_cow_copy_count : 0;
}

uint64_t pagefault_debug_read_pte(uint64_t pml4_phys, uint64_t addr)
{
    uint64_t *pt_entry = walk_to_pt_entry(pml4_phys, addr);
    return pt_entry ? *pt_entry : 0;
}
