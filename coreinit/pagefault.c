#include "pagefault.h"
#include "irq.h"
#include "serial.h"

#define MAX_STREAMED_PAGES 16
#define MAX_COW_PAGES 16
#define COW_POOL_SIZE 4

#define PAGEFAULT_VECTOR 0x0E /* CPU exception 14, #PF -- fixed by the
                               * x86-64 architecture itself, not a
                               * choice this project makes the way the
                               * LAPIC timer's vector 0x22 was */

/* A fresh page table for one split 2MB region -- static, since this
 * project has no general physical-page allocator yet (see the header's
 * own SCOPE note); one is enough for this demonstration's one region. */
static uint64_t g_split_pt[512] __attribute__((aligned(4096)));
static uint64_t g_split_region_base = 0;
static int g_have_split = 0;

static uint64_t g_streamed_pages[MAX_STREAMED_PAGES];
static size_t g_streamed_count = 0;

static uint64_t g_cow_pages[MAX_COW_PAGES];
static size_t g_cow_count = 0;

static uint8_t g_cow_pool[COW_POOL_SIZE][4096] __attribute__((aligned(4096)));
static size_t g_cow_pool_used = 0;

static uint64_t g_fault_count = 0;
static uint64_t g_cow_copy_count = 0;

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
    uint64_t *pd_entry = walk_to_pd_entry(pml4_phys, region_base);
    if (!pd_entry || !(*pd_entry & PAGE_PRESENT) || !(*pd_entry & PAGE_HUGE)) {
        kprintf("[coreinit]   pagefault: split failed, region 0x%X isn't "
                "mapped as a huge page\n",
                (unsigned)region_base);
        return 0;
    }

    uint64_t huge_base = *pd_entry & ~0x1FFFFFull; /* 2MB-aligned */
    for (int i = 0; i < 512; i++) {
        g_split_pt[i] = (huge_base + (uint64_t)i * 4096) | PAGE_PRESENT |
                       PAGE_WRITABLE;
    }

    *pd_entry = (uint64_t)(uintptr_t)g_split_pt | PAGE_PRESENT | PAGE_WRITABLE;
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

    g_split_region_base = huge_base;
    g_have_split = 1;
    kprintf("[coreinit]   pagefault: split 2MB region 0x%X into a real "
            "4KB-granular mapping, identical access preserved\n",
            (unsigned)huge_base);
    return 1;
}

void pagefault_register_streamed_page(uint64_t page_addr)
{
    if (g_streamed_count < MAX_STREAMED_PAGES) {
        g_streamed_pages[g_streamed_count++] = page_addr;
    }
}

static int is_registered_streamed(uint64_t addr)
{
    for (size_t i = 0; i < g_streamed_count; i++) {
        if (g_streamed_pages[i] == addr) {
            return 1;
        }
    }
    return 0;
}

static int is_registered_cow(uint64_t addr)
{
    for (size_t i = 0; i < g_cow_count; i++) {
        if (g_cow_pages[i] == addr) {
            return 1;
        }
    }
    return 0;
}

int pagefault_mark_absent(uint64_t pml4_phys, uint64_t page_addr)
{
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
    if (g_cow_count < MAX_COW_PAGES) {
        g_cow_pages[g_cow_count++] = page_addr;
    }
    kprintf("[coreinit]   pagefault: page 0x%X marked copy-on-write -- "
            "reads free, first write will fault and copy\n",
            (unsigned)page_addr);
    return 1;
}

static void resolve_streamed(uint64_t fault_addr, uint64_t pml4_phys)
{
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

static void resolve_cow_write(uint64_t fault_addr, uint64_t pml4_phys)
{
    uint64_t *pt_entry = walk_to_pt_entry(pml4_phys, fault_addr);
    if (!pt_entry) {
        return;
    }

    if (g_cow_pool_used >= COW_POOL_SIZE) {
        kprintf("[coreinit]   pagefault: COW pool exhausted, cannot "
                "resolve write to 0x%X\n",
                (unsigned)fault_addr);
        return;
    }

    const uint8_t *shared_source =
        (const uint8_t *)(uintptr_t)(*pt_entry & PAGE_ADDR_MASK);
    uint8_t *private_copy = g_cow_pool[g_cow_pool_used++];
    for (int i = 0; i < 4096; i++) {
        private_copy[i] = shared_source[i];
    }

    *pt_entry = ((uint64_t)(uintptr_t)private_copy & PAGE_ADDR_MASK) |
               PAGE_PRESENT | PAGE_WRITABLE;
    invlpg(fault_addr);
    g_cow_copy_count++;
    kprintf("[coreinit]   pagefault: COW copy made for 0x%X -- this "
            "context now has its own private page, the shared original "
            "is untouched\n",
            (unsigned)fault_addr);
}

static uint64_t g_pagefault_pml4_phys;

__attribute__((interrupt)) static void isr_page_fault(struct interrupt_frame *frame,
                                                       uint64_t error_code)
{
    (void)frame;
    uint64_t fault_addr;
    __asm__ volatile("mov %%cr2, %0" : "=r"(fault_addr));

    uint64_t page_addr = fault_addr & ~0xFFFull;
    int was_present = (error_code & 0x1) != 0;
    int was_write = (error_code & 0x2) != 0;

    g_fault_count++;

    if (!was_present && is_registered_streamed(page_addr)) {
        resolve_streamed(page_addr, g_pagefault_pml4_phys);
        return;
    }
    if (was_present && was_write && is_registered_cow(page_addr)) {
        resolve_cow_write(page_addr, g_pagefault_pml4_phys);
        return;
    }

    kprintf("[coreinit] UNHANDLED PAGE FAULT at 0x%X, error_code=0x%X "
            "(present=%d write=%d) -- halting\n",
            (unsigned)fault_addr, (unsigned)error_code, was_present,
            was_write);
    for (;;) {
        __asm__ volatile("hlt");
    }
}

void pagefault_init(uint64_t pml4_phys)
{
    g_pagefault_pml4_phys = pml4_phys;
    irq_install_gate(PAGEFAULT_VECTOR, (void *)isr_page_fault);

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
    return g_fault_count;
}

uint64_t pagefault_get_cow_copy_count(void)
{
    return g_cow_copy_count;
}

uint64_t pagefault_debug_read_pte(uint64_t pml4_phys, uint64_t addr)
{
    uint64_t *pt_entry = walk_to_pt_entry(pml4_phys, addr);
    return pt_entry ? *pt_entry : 0;
}
