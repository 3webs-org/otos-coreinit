/* Per-core ARMv8 STREAM/COW faults. Each init image owns its own TTBR0
 * root and these two 4 KiB tables: one L2 table replaces the 1 GiB RAM
 * block, then one L3 table splits a single 2 MiB region into pages.
 * All other identity mappings are left alone. */
#include "pagefault.h"
#include "arch_mmu.h"
#include "serial.h"

#define PAGE_SIZE 4096ull
#define REGION_SIZE 0x200000ull
#define RAM_BASE 0x40000000ull
#define RAM_END 0x80000000ull
#define ADDR_MASK 0x0000FFFFFFFFF000ull
#define VALID 0x1ull
#define TABLE_OR_PAGE 0x3ull
#define ACCESS_FLAG (1ull << 10)
#define NORMAL_MEMORY (1ull << 2)
#define INNER_SHAREABLE (3ull << 8)
#define UXN (1ull << 54)
#define READ_ONLY (1ull << 7) /* AP[2:1] = 10: EL1 read-only */
#define MAX_STREAMED_PAGES 16
#define MAX_COW_PAGES 16
#define COW_POOL_SIZE 4

static uint64_t g_l2[512] __attribute__((aligned(4096)));
static uint64_t g_l3[512] __attribute__((aligned(4096)));
static uint8_t g_cow_pool[COW_POOL_SIZE][4096] __attribute__((aligned(4096)));
static uint64_t g_split_base;
static int g_have_split, g_handler_ready;
static uint64_t g_streamed[MAX_STREAMED_PAGES], g_cow[MAX_COW_PAGES];
static size_t g_streamed_count, g_cow_count, g_cow_used;
static uint64_t g_fault_count, g_cow_copies;
static pagefault_el0_hook_t g_el0_hook;
extern char irq_aarch64_vectors[];

void pagefault_aarch64_set_el0_hook(pagefault_el0_hook_t hook)
{
    __atomic_store_n(&g_el0_hook, hook, __ATOMIC_RELEASE);
}

static void flush_translations(void)
{
    __asm__ volatile("dsb ishst\n\ttlbi vmalle1is\n\tdsb ish\n\tisb"
                     ::: "memory");
}

static uint64_t *page_entry(uint64_t addr)
{
    if (!g_have_split || addr < g_split_base ||
        addr >= g_split_base + REGION_SIZE) {
        return NULL;
    }
    return &g_l3[(addr >> 12) & 511u];
}

static int contains(const uint64_t *pages, size_t count, uint64_t page)
{
    for (size_t i = 0; i < count; i++) {
        if (pages[i] == page) {
            return 1;
        }
    }
    return 0;
}

int pagefault_supported(void)
{
    return arch_has_mmu() && arch_mmu_root_phys() != 0;
}

void pagefault_init(uint64_t pml4_phys)
{
    (void)pml4_phys; /* x86's PML4 is not an ARM TTBR. */
    if (!pagefault_supported()) {
        return;
    }
    __asm__ volatile("msr vbar_el1, %0\n\tisb" ::
                     "r"((uint64_t)(uintptr_t)irq_aarch64_vectors) : "memory");
    g_handler_ready = 1;
}

int pagefault_split_region(uint64_t pml4_phys, uint64_t region_base)
{
    (void)pml4_phys;
    uint64_t root_phys = arch_mmu_root_phys();
    if (!g_handler_ready || !root_phys || (region_base & (REGION_SIZE - 1)) ||
        region_base < RAM_BASE || region_base >= RAM_END || g_have_split) {
        return 0;
    }
    uint64_t *l1 = (uint64_t *)(uintptr_t)root_phys;
    if ((l1[1] & 3u) != VALID) {
        return 0;
    }
    const uint64_t flags = VALID | NORMAL_MEMORY | INNER_SHAREABLE |
                           ACCESS_FLAG | UXN;
    for (unsigned i = 0; i < 512; i++) {
        g_l2[i] = (RAM_BASE + (uint64_t)i * REGION_SIZE) | flags;
    }
    /* Replace the 1 GiB block, preserving every other 2 MiB translation.
     * Install the target's L3 table only after all page descriptors exist. */
    l1[1] = (uint64_t)(uintptr_t)g_l2 | TABLE_OR_PAGE;
    for (unsigned i = 0; i < 512; i++) {
        g_l3[i] = (region_base + (uint64_t)i * PAGE_SIZE) |
                  flags | TABLE_OR_PAGE;
    }
    g_l2[(region_base >> 21) & 511u] =
        (uint64_t)(uintptr_t)g_l3 | TABLE_OR_PAGE;
    g_split_base = region_base;
    g_have_split = 1;
    flush_translations();
    kprintf("[coreinit]   pagefault: ARM split 2 MiB region 0x%llX into 4 KiB pages\n",
            (unsigned long long)region_base);
    return 1;
}

void pagefault_register_streamed_page(uint64_t page_addr)
{
    if (!contains(g_streamed, g_streamed_count, page_addr) &&
        g_streamed_count < MAX_STREAMED_PAGES) {
        g_streamed[g_streamed_count++] = page_addr;
    }
}

int pagefault_mark_absent(uint64_t pml4_phys, uint64_t page_addr)
{
    (void)pml4_phys;
    uint64_t *pte = page_entry(page_addr);
    if (!g_handler_ready || !pte || !(*pte & VALID) ||
        (!contains(g_streamed, g_streamed_count, page_addr) &&
         g_streamed_count == MAX_STREAMED_PAGES)) {
        return 0;
    }
    pagefault_register_streamed_page(page_addr);
    *pte &= ~VALID; /* Preserve the backing physical address for retry. */
    flush_translations();
    return 1;
}

int pagefault_mark_cow(uint64_t pml4_phys, uint64_t page_addr)
{
    (void)pml4_phys;
    uint64_t *pte = page_entry(page_addr);
    if (!g_handler_ready || !pte || !(*pte & VALID) ||
        (!contains(g_cow, g_cow_count, page_addr) &&
         g_cow_count == MAX_COW_PAGES)) {
        return 0;
    }
    if (!contains(g_cow, g_cow_count, page_addr)) {
        g_cow[g_cow_count++] = page_addr;
    }
    *pte |= READ_ONLY;
    flush_translations();
    return 1;
}

/* Called by the synchronous EL1 vector. ESR_EL1.EC=0x25/0x24 identifies
 * same/lower-EL data aborts. DFSC=4..7 is translation fault; 12..15 is
 * permission fault, with WnR in ISS bit 6. ERET retries the original PC. */
void pagefault_aarch64_handle(uint64_t *frame)
{
    uint64_t esr, far, elr;
    __asm__ volatile("mrs %0, esr_el1" : "=r"(esr));
    __asm__ volatile("mrs %0, far_el1" : "=r"(far));
    __asm__ volatile("mrs %0, elr_el1" : "=r"(elr));
    uint64_t ec = (esr >> 26) & 0x3fu;
    /* EL0 SVC, Data Abort and BRK belong to the wrapper that created the
     * privilege boundary, not to coreinit's STREAM/COW policy. */
    pagefault_el0_hook_t hook = __atomic_load_n(&g_el0_hook, __ATOMIC_ACQUIRE);
    if (hook && (ec == 0x15 || ec == 0x24 || ec == 0x3c) &&
        hook(frame, esr, far)) {
        return;
    }
    uint64_t dfsc = esr & 0x3fu;
    uint64_t page = far & ~(PAGE_SIZE - 1);
    if (g_handler_ready && (ec == 0x25 || ec == 0x24)) {
        g_fault_count++;
        uint64_t *pte = page_entry(page);
        if (pte && dfsc >= 4 && dfsc <= 7 &&
            contains(g_streamed, g_streamed_count, page) && !(*pte & VALID)) {
            *pte |= VALID;
            flush_translations();
            return;
        }
        if (pte && dfsc >= 12 && dfsc <= 15 && (esr & (1ull << 6)) &&
            contains(g_cow, g_cow_count, page) && (*pte & READ_ONLY) &&
            g_cow_used < COW_POOL_SIZE) {
            const uint8_t *src = (const uint8_t *)(uintptr_t)(*pte & ADDR_MASK);
            uint8_t *dst = g_cow_pool[g_cow_used++];
            for (unsigned i = 0; i < 4096; i++) {
                dst[i] = src[i];
            }
            *pte = (*pte & ~(ADDR_MASK | READ_ONLY)) |
                   ((uint64_t)(uintptr_t)dst & ADDR_MASK);
            flush_translations();
            g_cow_copies++;
            return;
        }
    }
    kprintf("[coreinit] UNHANDLED ARM exception: ESR=0x%llX FAR=0x%llX ELR=0x%llX\n",
            (unsigned long long)esr, (unsigned long long)far,
            (unsigned long long)elr);
    __asm__ volatile("msr daifset, #0xf" ::: "memory");
    for (;;) {
        __asm__ volatile("wfe");
    }
}

uint64_t pagefault_get_cow_copy_count(void)
{
    return g_cow_copies;
}

uint64_t pagefault_get_count(void)
{
    return g_fault_count;
}

uint64_t pagefault_debug_read_pte(uint64_t pml4_phys, uint64_t addr)
{
    (void)pml4_phys;
    uint64_t *pte = page_entry(addr);
    return pte ? *pte : 0;
}
