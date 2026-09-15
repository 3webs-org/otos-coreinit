#pragma once

#include "cap.h"

/* A capability object that tracks which physical memory regions are
 * claimed, and by what. Method 0 (claim) is the original, MAP_FIXED-
 * style entry point -- caller supplies the exact base it wants,
 * rejected if it overlaps an existing claim -- still what smp.c uses
 * for each AP's region, since those addresses are a well-known formula
 * (CORE_REGION_BASE + i*CORE_REGION_SIZE) the trampoline's own fixed
 * offsets depend on, not something that can float.
 *
 * Methods 1/2 (alloc/free) are the actual allocator: mmap, not brk --
 * caller supplies only a size, this object finds a free gap anywhere
 * in the allocatable window and returns its base, and free releases a
 * previously-allocated base so that same space becomes available again
 * for a later, differently-sized allocation. This exists so "which
 * physical address does this slice of memory live at" stops being
 * something every caller in this project picks by hand and justifies
 * with a comment (see this project's own git history for exactly that
 * pattern, repeated at 0x800000, 0xA00000, and elsewhere) -- a single
 * memory device, subdivided into slices on demand, is meant to make a
 * job's own memory nothing more than "a bunch of bits backed by...
 * something," the something being this object's concern, not the
 * job's.
 *
 * Method 0 = claim: {base: u64, size: u64, kind: u64} -> {ok: u64} (1 =
 * recorded, 0 = rejected because it overlaps an existing claim -- kind
 * is an opaque tag the caller supplies for its own bookkeeping; this
 * object doesn't interpret it beyond storing it, see MEM_KIND_* below
 * for the conventions this project's own callers use).
 * Method 1 = alloc: {size: u64, kind: u64} -> {ok: u64, base: u64}.
 * First-fit within the allocatable window (see ALLOC_MIN/ALLOC_MAX in
 * mem_mgr.c) -- ok=0 if no gap of that size exists anywhere in the
 * window.
 * Method 2 = free: {base: u64} -> {ok: u64}. Only releases a claim
 * previously made through alloc (or claim); ok=0 if base doesn't match
 * any current claim's own base exactly. */
extern object_schema_t g_memory_manager_schema;
void memory_manager_handler(void *data, uint32_t method_id, const void *msg,
                            size_t len, void *response, size_t response_cap,
                            size_t *out_response_len);

void memory_manager_init_schema(void);

#define MEM_KIND_MMIO 0
#define MEM_KIND_KERNEL_RESERVED 1
#define MEM_KIND_CORE_REGION 2
#define MEM_KIND_JOB_SLICE 3
