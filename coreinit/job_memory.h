#pragma once

#include <stdint.h>
#include "scheduler.h"

/* A job's memory slice: a directly-usable virtual address plus enough
 * metadata to release it later, regardless of whether the underlying
 * bytes are local physical memory this node owns outright, or memory
 * streamed/COW-mapped in from another node's ownership via pagefault.c.
 * From a job's point of view, memory is "a bunch of bits backed by...
 * something" -- where those bits live and how they got mapped in are
 * decisions made once, here, not something every job reasons about on
 * every access.
 *
 * A slice acquired via job_memory_try_acquire_local is owned by the
 * acquiring JOB specifically, not merely attributed to its node: the
 * resource's own reserved_by is set to that job's id at the moment of
 * creation (see cluster_scheduler.c's handle_allocate_slice), so no
 * other job's resource_kind==MEMORY match can grab it out from under
 * the job that just allocated it. Ownership persists past that single
 * job invocation -- same as any other explicitly-reserved resource in
 * this project, nothing auto-frees it -- until job_memory_try_release
 * is called (by a job whose id matches reserved_by; anyone else's
 * release attempt is refused) or the resource_id is deliberately
 * handed to a different job. */
typedef struct {
    uint64_t resource_id;
    uint64_t base; /* usable address -- identity-mapped, virtual == physical */
    uint64_t size;
    uint64_t sync_kind;
    int owned; /* 1 if this node is the resource's owner */
} job_memory_slice_t;

/* Must be called once per core, early in coreinit_main (both BSP and
 * AP paths), before any other job_memory_* call on that core -- this
 * is this core's own pml4_phys, needed for a STREAM/COW slice's local
 * fault setup (pagefault_init's #PF gate and CR0.WP are per-core; every
 * core needs its own). */
void job_memory_init(uint64_t my_pml4_phys);

/* Tries to acquire a fresh, local slice (RESOURCE_SYNC_TRANSFER) of the
 * given size from the pool -- the actual allocator call, guarded by
 * ALLOCATOR_MUTEX_ID. This is meant to be called from inside a job's
 * own fn, and its return value is meant to become the job's own return
 * value directly:
 *
 *   JOB_BLOCKED_ON_MUTEX  -- the allocator is busy elsewhere;
 *                            *out_block_id is already set to
 *                            ALLOCATOR_MUTEX_ID. Return this from the
 *                            job immediately -- the scheduler will run
 *                            other work on this core and retry the job
 *                            later. Never spins.
 *   JOB_FINISHED          -- the mutex was acquired and the allocation
 *                            was ATTEMPTED. out->resource_id == 0 means
 *                            it failed (pool exhausted, not mutex
 *                            contention) -- check that, don't assume
 *                            success just because this returned.
 *
 * Never returns JOB_RUNNING or JOB_BLOCKED_ON_JOB. */
job_result_t job_memory_try_acquire_local(uint64_t size, uint64_t kind,
                                          uint64_t self_job_id,
                                          uint64_t *out_block_id,
                                          job_memory_slice_t *out);

/* Same blocking contract as job_memory_try_acquire_local, for releasing
 * a slice this node owns outright back to the pool. out_ok is set to
 * whether the release itself succeeded once JOB_FINISHED is returned;
 * as with acquire, a JOB_FINISHED return doesn't by itself mean the
 * release succeeded. */
job_result_t job_memory_try_release(uint64_t resource_id, uint64_t self_job_id,
                                    uint64_t *out_block_id, int *out_ok);

/* Acquires access to an EXISTING resource by id, regardless of who owns
 * it or how it's meant to be synced -- does NOT touch the allocator or
 * its mutex at all (no mem_mgr call happens here). If this node already
 * owns the resource, the slice is immediately usable as-is. If it's
 * RESOURCE_SYNC_STREAM or RESOURCE_SYNC_COW and owned by a different
 * node, this does the local pagefault_split_region +
 * mark_absent/mark_cow setup on THIS node's own page tables, so the
 * returned base, once touched, transparently faults, resolves, and
 * behaves like ordinary memory from then on. Fails (returns 0) for a
 * RESOURCE_SYNC_TRANSFER resource this node doesn't own -- there is no
 * fault-driven path for that case; ownership has to move first via
 * scheduler_reassign_resource. Synchronous, non-blocking -- there is no
 * mutex to contend for here. */
int job_memory_acquire_resource(uint64_t resource_id, job_memory_slice_t *out);
