#pragma once

#include <stddef.h>
#include <stdint.h>

/* Real, page-fault-driven demand mapping -- this is how swap actually
 * avoids a syscall on every memory access, and it's the right mechanism
 * for a STREAMED resource (see cluster_scheduler.h's transfer-vs-stream
 * distinction) too: instead of an explicit invoke() on every touch, the
 * backing page starts marked NOT PRESENT, the CPU's own hardware page
 * fault trap catches the first access for free, our handler resolves it
 * (maps the page in) exactly once, and every access after that runs at
 * ordinary memory speed with zero involvement from this code at all.
 * The instruction that faulted is transparently retried by the CPU once
 * the handler returns -- this is what makes page faults different from
 * an ordinary exception: they're precisely restartable by construction.
 *
 * SCOPE: x86 boots with 2 MiB huge pages; ARM's QEMU virt reference
 * init builds a 1 GiB RAM block and splits that into 2 MiB blocks on
 * demand. pagefault_split_region changes ONE requested 2 MiB block to
 * 4 KiB pages with the same identity translations before marking any
 * page absent or read-only. The rest of the map stays unchanged.
 *
 * Also stated plainly: in this project's current single-box,
 * shared-memory architecture, "resolving" a fault on a streamed page
 * means the data is ALREADY physically present (nothing to fetch, just
 * a page table entry to install) -- because every core already shares
 * the same physical memory. The fault-trap-resolve-retry STRUCTURE
 * built here is genuinely the real mechanism and is what a future
 * cross-machine transport would slot into (fetch the page's current
 * contents over the network into a freshly claimed local physical
 * frame, THEN map it present) -- but that fetch step doesn't exist yet,
 * because no non-shared-memory transport exists yet either. */

/* Whether THIS node has page-fault mechanics at all. Fault-driven
 * STREAM/COW needs an MMU AND an implementation of this seam; a node
 * without the implementation reports 0 and every caller gates on it.
 * See arch_mmu.h, which anticipates exactly this for nodes without an
 * MMU. */
int pagefault_supported(void);

/* x86 page-table bit names; the ARM backend owns separate descriptor
 * flags and returns its raw L3 descriptor from debug_read_pte. */
#define PAGE_PRESENT 0x1ull
#define PAGE_WRITABLE 0x2ull
#define PAGE_HUGE 0x80ull
#define PAGE_ADDR_MASK 0x000FFFFFFFFFF000ull

/* x86 installs #PF vector 14 in its IDT. ARM installs its synchronous
 * EL1 Data Abort handler in the per-core VBAR_EL1 vector table. */
void pagefault_init(uint64_t pml4_phys);

/* Splits the 2MB huge-page region containing `region_base` (must
 * already be 2MB-aligned) into a real 4KB-granular mapping, identical
 * in effect to the huge page it replaces (every sub-page present,
 * writable, at its correct identity-mapped physical address) until
 * something explicitly marks one absent. Returns 1 on success, 0 if
 * the region wasn't found mapped as a huge page (already split, or not
 * mapped at all) -- safe to check before assuming this is a fresh
 * split. */
int pagefault_split_region(uint64_t pml4_phys, uint64_t region_base);

/* Marks one 4KB page within an ALREADY-SPLIT region as not present --
 * the next access to it will fault. Returns 1 on success, 0 if the page
 * isn't within a split region (call pagefault_split_region on its
 * containing 2MB region first). */
int pagefault_mark_absent(uint64_t pml4_phys, uint64_t page_addr);

/* Registers `page_addr` (must be 4KB-aligned) as a streamed page this
 * core knows how to resolve on fault -- called once per page before
 * marking it absent. In this project's current single-box model,
 * resolution is always "just mark it present again"; a real
 * cross-machine implementation would fetch the page's contents first.
 * Called by pagefault's own ISR when a registered page faults; not
 * meant to be called directly. */
void pagefault_register_streamed_page(uint64_t page_addr);

/* ---- Copy-on-write: the third sync pattern, sharing this same fault
 * trap but the OTHER kind of fault it can report -- a page marked
 * present but read-only faults on a WRITE attempt (error code bit 0 set
 * = protection violation, not "not present"), rather than on every
 * access the way a streamed page does. Reads to a COW page cost
 * nothing at all, ever -- no fault, no handler involvement, ordinary
 * memory speed, because a read never violates the read-only
 * protection. Only the FIRST write faults; the handler allocates a
 * fresh physical page, copies the shared page's current contents into
 * it, retargets THIS faulting context's page table entry (and only
 * this one) at the new, private, writable copy, and retries -- every
 * other node or context still sharing the original page is completely
 * unaffected, which is the whole point: divergence is paid for only by
 * whoever actually diverges, and only once. ----
 *
 * SCOPE: the "fresh physical page" comes from a small, fixed,
 * dedicated pool (see pagefault.c) -- this is a demonstration of the
 * real mechanism, not a general physical page allocator; a production
 * version would draw from the same resource/memory-manager machinery
 * everything else in this project already uses for claiming memory. */

/* Marks an already-mapped, present page as copy-on-write: read-only,
 * with its ORIGINAL physical backing left as the shared source every
 * COW-mapped reader defers to until (and unless) one of them writes.
 * Returns 1 on success, 0 if the page isn't within a split region
 * (call pagefault_split_region on its containing 2MB region first). */
int pagefault_mark_cow(uint64_t pml4_phys, uint64_t page_addr);

uint64_t pagefault_get_cow_copy_count(void);
uint64_t pagefault_get_count(void);

/* Diagnostic only: returns the raw PTE value for an address within a
 * split region (0 if not found), so callers can inspect exactly what
 * the page table says before/after an operation. */
uint64_t pagefault_debug_read_pte(uint64_t pml4_phys, uint64_t addr);

#if defined(__x86_64__)
/* The ring-dropping wrapper owns user #PF policy. The x86 backend routes
 * user faults through this trusted callback before its STREAM/COW logic.
 * frame[0..4] = RIP, CS, RFLAGS, user RSP, user SS. */
typedef int (*pagefault_ring3_hook_t)(uint64_t *frame,
                                      uint64_t error_code, uint64_t cr2);
void pagefault_x86_64_set_ring3_hook(pagefault_ring3_hook_t hook);
#endif

#if defined(__aarch64__)
/* A trusted wrapper object registers its OWN lower-EL synchronous policy
 * callback through a factory-created exception-router object. The hardware
 * vector only saves this frame and dispatches; it does not authorize calls.
 * frame[0..30] are x0..x30, frame[31] ELR_EL1, frame[32] SPSR_EL1. */
typedef int (*pagefault_el0_hook_t)(uint64_t *frame, uint64_t esr, uint64_t far);
void pagefault_aarch64_set_el0_hook(pagefault_el0_hook_t hook);
#endif
