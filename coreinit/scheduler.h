#pragma once

#include "cap.h"

/* A thin, per-node client of the main cluster scheduler
 * (cluster_scheduler.h) -- there is no local job queue or local
 * priority logic here anymore. Every dispatch decision, every mutex,
 * every bit of blocking state lives centrally; this file's only job is
 * to ask "what's next for me", run it, and report back.
 *
 * A job's fn is called once per slice and returns:
 *   JOB_RUNNING           -- more work remains, reschedule me
 *   JOB_FINISHED          -- done
 *   JOB_BLOCKED_ON_MUTEX  -- I tried scheduler_mutex_try_lock and lost;
 *                            *out_block_id is the mutex id
 *   JOB_BLOCKED_ON_JOB    -- I depend on another job finishing first;
 *                            *out_block_id is that job's id
 */

typedef enum {
    JOB_RUNNING = 0,
    JOB_FINISHED = 1,
    JOB_BLOCKED_ON_MUTEX = 2,
    JOB_BLOCKED_ON_JOB = 3,
} job_result_t;

typedef job_result_t (*job_fn_t)(void *ctx, uint64_t self_job_id,
                                 uint64_t *out_block_id);

/* A small, fixed vocabulary of job types -- every node registers the
 * SAME (type, fn) pairs during its own bootstrap, since every node runs
 * a byte-identical compiled binary. This is what makes cross-node job
 * dispatch safe without ever shipping a raw function pointer between
 * nodes: a job_type value is just a small integer, resolved through
 * each node's own local table. */
typedef enum {
    JOB_TYPE_PCI_SCAN = 0,
    JOB_TYPE_DISK_DEMO = 1,
    JOB_TYPE_TIMER_BRINGUP = 2,
    JOB_TYPE_SMP_BRINGUP = 3,
    JOB_TYPE_GREET_FROM_BSP = 4,
    JOB_TYPE_RESOURCE_DEMO = 5,
    JOB_TYPE_PAGEFAULT_DEMO = 6,
    JOB_TYPE_STREAM_SETUP = 7,
    JOB_TYPE_STREAM_COW_VERIFY = 8,
    JOB_TYPE_COUNT = 9,
} job_type_t;

/* Registers the LOCAL function for a job type. Called once per node,
 * with the identical set of registrations everywhere. */
void scheduler_register_job_type(job_type_t type, job_fn_t fn);

/* Tells this node how to reach the cluster scheduler (local invoke if
 * this IS the BSP, remote invoke through the BSP's own kernel_invoke
 * otherwise) and what this node's own id is. Called once, during this
 * node's own bootstrap, before submitting anything or calling
 * scheduler_run. */
void scheduler_client_init(
    int (*invoke_fn)(capability_t target, uint32_t method_id,
                    const void *msg, size_t len, void *response,
                    size_t response_cap, size_t *out_response_len),
    capability_t cluster_scheduler, uint64_t my_node_id);

/* Submits a job to run on THIS node (self-affinity) -- the ordinary
 * case, "I want this to happen here." ctx is an opaque uint64_t value;
 * safe as a raw pointer cast because self-affinity guarantees the same
 * node that created it is the one that will dereference it.
 *
 * Resource requirement: resource_id != 0 requires exactly that
 * resource; else resource_kind != RESOURCE_KIND_NONE requires any free
 * resource of that kind with capacity >= resource_min_capacity. Pass
 * (0, RESOURCE_KIND_NONE, 0) for no requirement at all. */
uint64_t scheduler_submit(job_type_t type, void *ctx, int priority,
                          uint64_t resource_id, uint64_t resource_kind,
                          uint64_t resource_min_capacity);

/* Submits a job targeting a SPECIFIC other node -- e.g. a BSP greeting a
 * newly-booted AP. ctx must be a plain value or a pointer into
 * genuinely shared memory (see cluster_scheduler.h's own note); a
 * pointer into the SUBMITTING node's private data would be exactly as
 * meaningless on the target as an untranslated function pointer. */
uint64_t scheduler_submit_to_node(uint64_t target_node_id, job_type_t type,
                                  uint64_t ctx, int priority,
                                  uint64_t resource_id, uint64_t resource_kind,
                                  uint64_t resource_min_capacity);

/* Declares a resource this node OWNS (a peripheral, most likely, at
 * this project's current stage -- CPU_CORE resources are created
 * automatically on join, MEMORY ones via scheduler_request_allocation)
 * and returns its cluster-wide id, for use in a later scheduler_submit
 * call. sync_kind is RESOURCE_SYNC_TRANSFER/STREAM/COW (see
 * cluster_scheduler.h) -- pass RESOURCE_SYNC_TRANSFER for the
 * project's original, ownership-moves-on-reassign behavior. */
uint64_t scheduler_declare_resource(uint64_t kind, uint64_t descriptor,
                                    uint64_t capacity, uint64_t sync_kind);

/* Reassigns an existing resource to a new owner node -- refused (returns
 * 0) if the resource is currently reserved by an active job. This is
 * the actual mechanism behind "the keyboard IRQ can be reassigned to any
 * node as needed": ownership moves, nothing about the resource's
 * identity (its resource_id) changes. Only meaningful for
 * RESOURCE_SYNC_TRANSFER resources -- a STREAM/COW resource's owner is
 * meant to stay put; non-owners access it via
 * scheduler_get_resource_info + pagefault.c instead. */
int scheduler_reassign_resource(uint64_t resource_id, uint64_t new_owner_node);

/* Claims a memory region through the cluster scheduler (which itself
 * wraps the memory manager's own claim ledger, adding node attribution)
 * and, on success, returns the resource id a job can declare it uses.
 * Returns 1/0 for success/failure, same as the memory manager's own
 * claim. sync_kind: see scheduler_declare_resource. */
int scheduler_request_allocation(uint64_t base, uint64_t size, uint64_t kind,
                                 uint64_t sync_kind, uint64_t *out_resource_id);

/* Looks up a resource's current metadata -- owner_node, kind,
 * descriptor, capacity, sync_kind -- without reserving or modifying
 * anything. This is how a non-owner node finds out a resource is
 * RESOURCE_SYNC_STREAM/COW, where its backing memory actually is
 * (descriptor for MEMORY resources is the base address, capacity the
 * size), and who still owns it, before setting up its own local
 * pagefault.c handling for that range. Returns 1 if the resource
 * exists, 0 otherwise (out fields are zeroed on failure). */
int scheduler_get_resource_info(uint64_t resource_id, uint64_t *out_owner_node,
                                uint64_t *out_kind, uint64_t *out_descriptor,
                                uint64_t *out_capacity,
                                uint64_t *out_sync_kind);

/* This core's own node id, as passed to scheduler_client_init -- for
 * code (job_memory.c, mainly) that needs to compare a resource's
 * owner_node against "am I the owner". */
uint64_t scheduler_get_my_node_id(void);

/* The actual allocator: caller supplies only a size and kind, gets back
 * a resource_id and the base address mem_mgr's own alloc chose --
 * "mmap, not brk," see mem_mgr.h. Unlike scheduler_request_allocation
 * (which validates a caller-chosen, fixed base -- still what smp.c
 * needs for each AP's formula-addressed region), this is the entry
 * point for "just give me N bytes, I don't care where." */
int scheduler_allocate_slice(uint64_t size, uint64_t kind, uint64_t sync_kind,
                             uint64_t self_job_id, uint64_t *out_resource_id,
                             uint64_t *out_base);

/* Releases a slice back to the pool. Must only be called while holding
 * ALLOCATOR_MUTEX_ID, and self_job_id must match the resource's own
 * reserved_by -- see scheduler.c's own note and job_memory_release, the
 * intended caller. */
int scheduler_release_slice(uint64_t resource_id, uint64_t self_job_id);

/* A scheduler-aware mutex, now cluster-wide: returns 1 if acquired, 0 if
 * already held by a different job anywhere in the cluster. A job's own
 * fn should return JOB_BLOCKED_ON_MUTEX with this mutex_id when it
 * loses. */
int scheduler_mutex_try_lock(int mutex_id, uint64_t self_job_id);
void scheduler_mutex_unlock(int mutex_id, uint64_t self_job_id);

/* Never returns. Called once, as this node's final bootstrap action:
 * asks the cluster scheduler for work, runs it, reports back, forever
 * -- falling back to hlt only when genuinely told there's nothing
 * available right now. */
void scheduler_run(void);
