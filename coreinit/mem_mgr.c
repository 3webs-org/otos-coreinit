#include "mem_mgr.h"
#include <ownership.h>
#include "capnp_build.h"
#include "schema.h"
#include "serial.h"
#include "arch_cpu.h"
#include "try_lock.h"
#include "../include/smp_layout.h"
#include "macho.h"

#define MAX_CLAIMS 64

/* The allocatable window for alloc/free (method 1/2), computed once in
 * memory_manager_init_schema from what this machine actually has, rather
 * than declared as a pair of constants. Nothing else may restate it --
 * cluster_scheduler.c asks via memory_manager_window(). */
static uint64_t g_alloc_lo;
static uint64_t g_alloc_hi;

typedef struct {
    uint64_t base;
    uint64_t size;
    uint64_t kind;
    int used;
} claim_t;

static claim_t g_claims[MAX_CLAIMS];
static size_t g_claim_count;
static trylock_t g_lock;

/* alloc_locked/claim_locked/free_locked are genuine multi-step
 * operations (scan for overlaps, then insert; or a first-fit search
 * across every existing claim) -- not reducible to a single atomic op
 * the way a plain counter bump is. Retries via hlt, not a spin: these
 * calls are rare (allocation events, not a per-tick hot path) and hlt
 * actually stops the core until the next interrupt rather than
 * burning cycles polling a flag. No job context here to job_exec_wait
 * on instead -- this runs as a plain synchronous kernel_invoke
 * handler, same reasoning as cluster_scheduler.c's own
 * storage_lock_acquire_wait. */
static void mem_lock_acquire_wait(void) lock_acquires_for_caller;
static void mem_lock_acquire_wait(void)
{
    while (!trylock_try_acquire(&g_lock)) {
        arch_idle();
    }
}


#define METHOD_CLAIM 0
#define METHOD_ALLOC 1
#define METHOD_FREE 2

static const struct_schema_t g_claim_request_schema = {
    .data_words = 3, .pointer_count = 0, .pointers = NULL};
static const struct_schema_t g_claim_result_schema = {
    .data_words = 1, .pointer_count = 0, .pointers = NULL};
static const struct_schema_t g_alloc_request_schema = {
    .data_words = 2, .pointer_count = 0, .pointers = NULL};
static const struct_schema_t g_alloc_result_schema = {
    .data_words = 2, .pointer_count = 0, .pointers = NULL};

static method_schema_t g_methods[3];
object_schema_t g_memory_manager_schema;

/* Shared by both the handler's CLAIM method and this file's own
 * init-time pre-registration of known-reserved regions -- one overlap
 * check, one insertion path, no duplicated logic between "claims made
 * through the capability interface" and "claims this object already
 * knows about before anyone asks." Caller must hold g_lock. */
fallible
static int claim_locked(uint64_t base, uint64_t size, uint64_t kind)
{
    uint64_t end = base + size;
    for (size_t i = 0; i < g_claim_count; i++) {
        if (!g_claims[i].used) {
            continue;
        }
        uint64_t existing_end = g_claims[i].base + g_claims[i].size;
        if (base < existing_end && g_claims[i].base < end) {
            return 0; /* overlap */
        }
    }
    if (g_claim_count >= MAX_CLAIMS) {
        return 0;
    }
    g_claims[g_claim_count].base = base;
    g_claims[g_claim_count].size = size;
    g_claims[g_claim_count].kind = kind;
    g_claims[g_claim_count].used = 1;
    g_claim_count++;
    return 1;
}

/* Caller must hold g_lock. */
fallible
static int alloc_locked(uint64_t size, uint64_t kind, uint64_t *out_base)
{
    /* mmap, not brk: any free gap is a valid placement, not just space
     * past a high-water mark -- so a freed slice's address range
     * genuinely becomes available again for a later, differently-sized
     * request, rather than being permanently spent. First-fit over the
     * candidate starts formed by ALLOC_MIN plus every existing claim's
     * own end address; MAX_CLAIMS is small enough that the O(n^2) scan
     * this implies is cheap and, more importantly, easy to trust. */
    uint64_t candidates[MAX_CLAIMS + 1];
    size_t n = 0;
    candidates[n++] = g_alloc_lo;
    for (size_t i = 0; i < g_claim_count; i++) {
        if (g_claims[i].used) {
            candidates[n++] = g_claims[i].base + g_claims[i].size;
        }
    }
    for (size_t i = 0; i < n; i++) {
        size_t min_idx = i;
        for (size_t j = i + 1; j < n; j++) {
            if (candidates[j] < candidates[min_idx]) {
                min_idx = j;
            }
        }
        uint64_t tmp = candidates[i];
        candidates[i] = candidates[min_idx];
        candidates[min_idx] = tmp;
    }

    for (size_t i = 0; i < n; i++) {
        uint64_t start =
            candidates[i] < g_alloc_lo ? g_alloc_lo : candidates[i];
        uint64_t end = start + size;
        if (end > g_alloc_hi || end < start) {
            continue; /* past the window, or overflowed */
        }
        int overlaps = 0;
        for (size_t j = 0; j < g_claim_count; j++) {
            if (!g_claims[j].used) {
                continue;
            }
            uint64_t existing_end = g_claims[j].base + g_claims[j].size;
            if (start < existing_end && g_claims[j].base < end) {
                overlaps = 1;
                break;
            }
        }
        if (!overlaps && claim_locked(start, size, kind)) {
            *out_base = start;
            return 1;
        }
    }
    return 0;
}

/* Caller must hold g_lock. */
fallible
static int free_locked(uint64_t base)
{
    for (size_t i = 0; i < g_claim_count; i++) {
        if (g_claims[i].used && g_claims[i].base == base) {
            g_claims[i].used = 0;
            return 1;
        }
    }
    return 0;
}

/* A pre-claim that says so when it fails. claim_locked returns 0 for two
 * reasons that both matter here -- the span overlaps something already
 * claimed, or the claim table is full -- and every caller below used to
 * discard that, so either one passed silently and left the span
 * unprotected from the allocator.
 *
 * Newly worth catching rather than merely tidy: these spans used to be
 * fixed constants that could be checked by reading the header, and are now
 * measured at runtime (the kernel's and coreinit's own images) with init
 * placed wherever the firmware reported free memory. An overlap is
 * something this boot can discover, not something the layout rules out in
 * advance. Caller must hold g_lock. */
static void claim_or_warn(uint64_t base, uint64_t size, uint64_t kind,
                          const char *what)
{
    if (!claim_locked(base, size, kind)) {
        kprintf("[coreinit]   memory_manager: WARNING: could not reserve %s "
                "at 0x%X-0x%X -- overlaps an existing claim, or the claim "
                "table is full. That span is NOT protected this boot.\n",
                what, (unsigned)base, (unsigned)(base + size));
    }
}

void memory_manager_window(uint64_t *out_base, uint64_t *out_size)
{
    *out_base = g_alloc_lo;
    *out_size = g_alloc_hi > g_alloc_lo ? g_alloc_hi - g_alloc_lo : 0;
}

void memory_manager_init_schema(uint64_t kernel_image_base,
                                uint64_t kernel_image_len,
                                uint64_t free_base, uint64_t free_size,
                                uint64_t self_image_base)
{
    trylock_init(&g_lock);
    g_claim_count = 0;

    g_methods[0].method_id = METHOD_CLAIM;
    g_methods[0].param_schema = &g_claim_request_schema;
    g_methods[0].result_schema = &g_claim_result_schema;
    g_methods[1].method_id = METHOD_ALLOC;
    g_methods[1].param_schema = &g_alloc_request_schema;
    g_methods[1].result_schema = &g_alloc_result_schema;
    g_methods[2].method_id = METHOD_FREE;
    g_methods[2].param_schema = &g_claim_result_schema; /* {base: u64} -- same 1-word shape */
    g_methods[2].result_schema = &g_claim_result_schema;

    g_memory_manager_schema.methods = g_methods;
    g_memory_manager_schema.method_count = 3;

    /* Known-reserved regions, claimed simply by what they are, before
     * anything else could possibly ask for them. Sizes are generous,
     * round numbers rather than exact image sizes -- this is bookkeeping
     * to catch collisions, not a tight-fit allocator. */
    mem_lock_acquire_wait();
    claim_or_warn(0xFEE00000, 0x1000, MEM_KIND_MMIO, "the local APIC");
    claim_or_warn(0xFEC00000, 0x1000, MEM_KIND_MMIO, "the IOAPIC");
    /* The kernel's REAL extent, measured by the kernel itself and handed
     * over in boot_info -- not a round number, and not an address this
     * file names. Rounded up to a page because a reservation covering
     * part of a page is a reservation that doesn't cover that page.
     * This replaces a generous 4MB guess whose justification (that the
     * kernel image carries coreinit embedded within it, via boot.S's
     * .incbin) stops being true the moment init is loaded as a
     * bootloader module instead -- another reason not to restate a size
     * that something else already knows. */
    if (kernel_image_len != 0) {
        uint64_t span = (kernel_image_len + 0xFFFull) & ~0xFFFull;
        claim_or_warn(kernel_image_base, span, MEM_KIND_KERNEL_RESERVED,
                      "the kernel image");
    } else {
        kprintf("[coreinit]   memory_manager: WARNING: kernel image extent "
                "unknown, its span is NOT reserved this boot\n");
    }
    claim_or_warn(0x8000, 0x1000, MEM_KIND_KERNEL_RESERVED,
                  "the AP trampoline page");
    /* coreinit's own loaded image, measured rather than declared. A
     * dynamic allocation landing here would silently corrupt the very
     * code doing the allocating, which is why this is claimed explicitly
     * at all. It used to be a constant address and a round 4 MiB of
     * "documented headroom"; both are now read off the image itself, so
     * growth needs no matching edit and cannot drift out of step. Page-
     * rounded, because a reservation covering part of a page does not
     * cover that page. */
    /* Where the kernel placed us, as reported -- NOT ld64's
     * _mh_execute_header, which compiles to a GOT-indirect load that
     * nothing rebases in a freestanding image, and so still reads the
     * link-time address once init is relocated. See init_abi.h. */
    uint64_t self_base = self_image_base;
    uint64_t self_len = self_base
        ? macho_image_size((const void *)(uintptr_t)self_base) : 0;
    if (self_len != 0) {
        claim_or_warn(self_base, (self_len + 0xFFFull) & ~0xFFFull,
                      MEM_KIND_KERNEL_RESERVED, "coreinit's own image");
    } else {
        /* Prints the address but does not read it: reaching here means it
         * is not a Mach-O header, and it may not be readable at all. */
        kprintf("[coreinit]   memory_manager: WARNING: no Mach-O header at "
                "the load base the kernel reported (0x%X), so coreinit's own "
                "span is NOT reserved this boot\n",
                (unsigned)self_base);
    }

    /* The allocatable window, derived instead of declared. It used to be
     * ALLOC_MIN/ALLOC_MAX -- 28 MiB and 64 MiB -- where 28 MiB existed only
     * to leave coreinit "4 MiB of headroom to grow into". Measuring the
     * image makes that margin unnecessary: growth moves this bound by
     * itself, and the 3+ MiB the margin was costing comes back.
     *
     * Bottom: just past our own image. Everything else the BSP places by
     * fixed address (its mailbox, discovery record, and page tables, all
     * around 18 MiB) sits below that, as does the kernel.
     *
     * Top: still CORE_REGION_BASE, because each AP's region is placed by
     * formula from it -- the one genuinely hardcoded bound left here, and
     * an architectural question rather than a constant to derive (see
     * TODO.md). Clamped down to the end of discovered free memory, so a
     * machine smaller than the formula assumes yields a smaller window
     * rather than addresses that are not backed by RAM at all. */
    uint64_t lo = self_len != 0
        ? ((self_base + self_len + 0xFFFull) & ~0xFFFull)
        : free_base;
    uint64_t hi = CORE_REGION_BASE;
    if (free_size != 0) {
        if (free_base > lo) {
            lo = free_base;
        }
        uint64_t free_end = free_base + free_size;
        if (free_end < hi) {
            hi = free_end;
        }
    }
    if (lo == 0 || hi <= lo) {
        g_alloc_lo = 0;
        g_alloc_hi = 0;
        kprintf("[coreinit]   memory_manager: WARNING: no usable allocatable "
                "window (lo 0x%X, hi 0x%X) -- every alloc will be refused\n",
                (unsigned)lo, (unsigned)hi);
    } else {
        g_alloc_lo = lo;
        g_alloc_hi = hi;
    }
    trylock_release(&g_lock);

    kprintf("[coreinit]   memory_manager: %u known region(s) pre-claimed, "
            "allocatable window 0x%X-0x%X (%u MiB)\n",
            (unsigned)g_claim_count, (unsigned)g_alloc_lo, (unsigned)g_alloc_hi,
            (unsigned)((g_alloc_hi - g_alloc_lo) / (1024 * 1024)));
}

void memory_manager_handler(void *data, uint32_t method_id, const void *msg,
                            size_t len, void *response, size_t response_cap,
                            size_t *out_response_len)
{
    (void)data;
    (void)len;
    *out_response_len = 0;
    const uint64_t *words = capnp_read_flat(msg);

    if (method_id == METHOD_ALLOC) {
        uint64_t size = words[0], kind = words[1];
        uint64_t base = 0;
        mem_lock_acquire_wait();
        int ok = alloc_locked(size, kind, &base);
        trylock_release(&g_lock);
        uint64_t out[2] = {ok ? 1u : 0u, base};
        *out_response_len = capnp_build_flat(response, response_cap, out, 2);
        if (ok) {
            kprintf("[coreinit]   memory_manager: allocated 0x%X-0x%X "
                    "(kind %u)\n",
                    (unsigned)base, (unsigned)(base + size), (unsigned)kind);
        } else {
            kprintf("[coreinit]   memory_manager: alloc REJECTED, no free "
                    "gap of size %u in the allocatable window\n",
                    (unsigned)size);
        }
        return;
    }

    if (method_id == METHOD_FREE) {
        uint64_t base = words[0];
        mem_lock_acquire_wait();
        int ok = free_locked(base);
        trylock_release(&g_lock);
        uint64_t result = ok ? 1u : 0u;
        *out_response_len = capnp_build_flat(response, response_cap, &result, 1);
        kprintf("[coreinit]   memory_manager: free 0x%X: %s\n",
                (unsigned)base, ok ? "released" : "no matching claim");
        return;
    }

    /* METHOD_CLAIM: MAP_FIXED-style, caller supplies the exact base. */
    uint64_t base = words[0];
    uint64_t size = words[1];
    uint64_t kind = words[2];

    mem_lock_acquire_wait();
    int ok = claim_locked(base, size, kind);
    trylock_release(&g_lock);

    uint64_t result = ok ? 1 : 0;
    *out_response_len = capnp_build_flat(response, response_cap, &result, 1);

    if (ok) {
        kprintf("[coreinit]   memory_manager: claimed 0x%X-0x%X (kind %u)\n",
                (unsigned)base, (unsigned)(base + size), (unsigned)kind);
    } else {
        kprintf("[coreinit]   memory_manager: REJECTED claim 0x%X-0x%X, "
                "overlaps an existing claim\n",
                (unsigned)base, (unsigned)(base + size));
    }
}
