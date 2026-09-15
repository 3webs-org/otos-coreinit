#include "mem_mgr.h"
#include "capnp_build.h"
#include "schema.h"
#include "serial.h"
#include "try_lock.h"
#include "../include/smp_layout.h"

#define MAX_CLAIMS 64

/* The allocatable window for alloc/free (method 1/2) -- see
 * smp_layout.h's ALLOC_MIN/ALLOC_MAX for the shared definition
 * cluster_scheduler.c's pool resource also uses. */

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
static void mem_lock_acquire_wait(void)
{
    while (!trylock_try_acquire(&g_lock)) {
        __asm__ volatile("hlt");
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
    candidates[n++] = ALLOC_MIN;
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
        uint64_t start = candidates[i] < ALLOC_MIN ? ALLOC_MIN : candidates[i];
        uint64_t end = start + size;
        if (end > ALLOC_MAX || end < start) {
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

void memory_manager_init_schema(void)
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
    claim_locked(0xFEE00000, 0x1000, MEM_KIND_MMIO);        /* local APIC */
    claim_locked(0xFEC00000, 0x1000, MEM_KIND_MMIO);        /* IOAPIC */
    /* Sized generously (4MB, matching BSP_INIT_LOAD_BASE's own
     * reservation below) rather than tightly: this kernel's own image
     * now carries coreinit's entire Mach-O embedded within it (see
     * boot.S's .incbin), not just its own compiled code, so its real
     * footprint is well past what a bare kernel binary alone would
     * need -- this is bookkeeping to catch collisions, not a tight
     * fit, so erring generous costs nothing but address space. */
    claim_locked(KERNEL_LOAD_BASE, 0x400000, MEM_KIND_KERNEL_RESERVED); /* BSP kernel image */
    claim_locked(0x8000, 0x1000, MEM_KIND_KERNEL_RESERVED); /* AP trampoline page */
    claim_locked(BSP_INIT_LOAD_BASE, 0x400000, MEM_KIND_KERNEL_RESERVED); /* BSP
                 * coreinit's own loaded image -- see smp_layout.h's own
                 * note on why this is claimed explicitly now: a future
                 * dynamic allocation landing here would silently
                 * corrupt coreinit's own running code, the same class
                 * of bug BSP_INIT_LOAD_BASE itself was introduced
                 * to fix. 4MB is coreinit's documented headroom, not
                 * its current size -- claiming the whole margin now
                 * means growth doesn't need a matching claim update. */
    trylock_release(&g_lock);

    kprintf("[coreinit]   memory_manager: %u known region(s) pre-claimed\n",
            (unsigned)g_claim_count);
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
