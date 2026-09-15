#pragma once

#include "cap.h"

/* The main cluster scheduler: the sole scheduling authority -- the
 * per-core scheduler that used to sit underneath this has been folded
 * in. Every node is either idle and asking "what's next for me", or
 * running a slice and about to report back. There is no local job queue
 * anywhere; a node's own dispatch loop (scheduler.h's scheduler_run) is
 * a thin client of this object.
 *
 * ---- Resources, one pattern for all kinds ----
 *
 * A resource has a kind (MEMORY, CPU_CORE, PERIPHERAL, ...), a
 * capacity (kind-specific -- byte size for MEMORY, 0/unused for the
 * others), a CURRENT OWNER node, and a transient reservation (which job,
 * if any, is actively using it right now -- 0 when free).
 *
 * Owner and reservation are deliberately separate, because they answer
 * different questions and change on different timescales. Ownership is
 * who the resource is currently assigned to and persists until someone
 * explicitly reassigns it (reassign_resource) -- this is what "the
 * keyboard IRQ can only be held by one node at a time, but can be
 * reassigned as needed" actually means: exactly one owner, changeable,
 * not tied to any job's lifetime. Reservation is which job is actively
 * using an owned resource right now, and is released the moment that
 * job finishes -- ordinary, transient exclusion, the same shape a mutex
 * has.
 *
 * A job declares what it needs one of two ways: a SPECIFIC resource_id
 * (I need exactly this one -- the one physical keyboard, an allocation
 * I already hold), or a KIND plus a minimum capacity (I need any free
 * resource of this kind with at least this much capacity). The second
 * form is what makes memory not a special case: "other nodes should be
 * able to declare additional memory resources" is just declare_resource
 * or request_allocation called again, from wherever the memory actually
 * is; "new allocations preferentially happening on this device" is
 * get_next_job's own resource-matching preferring the most recently
 * declared candidate; and "the existing stuff is treated like it's in
 * swap" is simply what already happens to any job whose resource
 * requirement can't currently be satisfied -- it's not dispatched,
 * exactly like a job waiting on a contended mutex, until something of
 * the right kind and size frees up or a new one is declared. No memory-
 * specific code exists anywhere in this dispatch path.
 *
 * A kind-based match is resolved AT MOST ONCE per job and then STICKS
 * for that job's entire lifetime, exactly like node affinity's own
 * ANY-then-pinned behavior: the first time get_next_job matches "any
 * memory" to a specific resource, that binding is permanent for this
 * job, not re-resolved on every subsequent dispatch. This matters for a
 * real reason, not just consistency -- a job that has already written
 * into a specific memory resource cannot be silently handed a
 * DIFFERENT one on its next slice; that would either lose its data or
 * require actually copying it, which is what real swap means and this
 * does not implement. If a job's already-bound resource somehow stops
 * being available, the job simply stays blocked (the honest "would need
 * real swap to proceed" case) rather than quietly resuming on the wrong
 * memory.
 *
 * Hosted on the BSP, like every other shared service so far, reached
 * the same way: local invoke if you ARE the BSP, remote invoke (through
 * a local proxy -- see proxy.h) otherwise.
 *
 * Method 0 = join_node: {node_id: u64, arch: u64, region_base: u64,
 * signal_target: u64 (architecture-opaque -- see cross_core_signal.h;
 * this is how the cluster later reaches this node for an immediate
 * cross-core preemption signal, not just a mailbox poll)} ->
 *   no response. Implicitly declares a CPU_CORE resource, owned by that
 *   node, for it too.
 *
 * Method 1 = leave_node: {node_id: u64} -> no response. Removes the
 *   node, and any resource it currently OWNS. Jobs whose node affinity
 *   REQUIRED that specific node are cancelled; jobs open to any node
 *   that happened to be assigned there are released back to the pool.
 *   Ordinary path, not just failure recovery -- hot-swap is normal.
 *   Detecting an ungraceful disappearance needs a liveness mechanism
 *   this milestone doesn't build.
 *
 * Method 2 = request_allocation: {node_id: u64, base: u64, size: u64,
 *   kind: u64} -> {ok: u64, resource_id: u64}. Wraps the memory
 *   manager's own claim (no node attribution of its own) and registers
 *   the claim as a MEMORY resource, owned by node_id, capacity = size.
 *
 * Method 3 = declare_resource: {node_id: u64, kind: u64, descriptor:
 *   u64, capacity: u64} -> {resource_id: u64}. General registration for
 *   anything that isn't a memory claim -- a peripheral, once this
 *   project has real device objects to back one with. Owned by node_id.
 *
 * Method 4 = reassign_resource: {resource_id: u64, new_owner_node: u64}
 *   -> {ok: u64}. Changes who owns a resource. Refused (ok=0) if the
 *   resource is currently reserved by an active job -- ownership can't
 *   be yanked out from under work in progress; wait for it to finish
 *   (or block on it) first.
 *
 * Method 5 = submit_job: {node_affinity: u64, job_type: u64, ctx: u64,
 *   priority: u64, resource_id: u64, resource_kind: u64,
 *   resource_min_capacity: u64} -> {job_id: u64}. resource_id != 0
 *   requires exactly that resource; else resource_kind !=
 *   RESOURCE_KIND_NONE requires any free, owned-by-someone resource of
 *   that kind with capacity >= resource_min_capacity.
 *
 * Method 6 = get_next_job: {requesting_node_id: u64} -> {found: u64,
 *   job_id: u64, job_type: u64, ctx: u64}. Picks the
 *   highest-effective-priority eligible, unblocked job for that node
 *   whose resource requirement (if any) is currently satisfiable;
 *   reserves the matched resource to this job's id.
 *
 * Method 7 = report_job_result: {job_id: u64, result: u64, block_id:
 *   u64} -> no response. Releases the job's resource reservation when
 *   result is JOB_FINISHED.
 *
 * Method 8 = mutex_try_lock: {mutex_id: u64, requesting_job_id: u64} ->
 *   {ok: u64}. Priority inheritance operates cluster-wide.
 *
 * Method 9 = mutex_unlock: {mutex_id: u64, requesting_job_id: u64} ->
 *   no response. */

#define CLUSTER_ARCH_X86_64 0
#define NODE_AFFINITY_ANY 0xFFFFFFFFFFFFFFFFull

#define RESOURCE_KIND_NONE 0xFFFFFFFFFFFFFFFFull /* "no kind requirement" sentinel */
#define RESOURCE_KIND_MEMORY 0
#define RESOURCE_KIND_CPU_CORE 1
#define RESOURCE_KIND_PERIPHERAL 2
#define RESOURCE_KIND_MMU 3 /* declared automatically at join, one per
                            * node that has one (see handle_join_node
                            * and arch_mmu.h) -- not something a node
                            * declares itself the way PERIPHERAL is.
                            * Its presence (or absence) is what
                            * RESOURCE_SYNC_STREAM/COW declarations are
                            * checked against: both rely entirely on
                            * pagefault.c's page-fault-driven mechanism,
                            * which simply doesn't exist on a node
                            * without one. */

/* sync_kind: how a resource is meant to be accessed by a node other
 * than its owner -- the property this project's earlier "transfer vs
 * streaming vs COW" discussion asked for, now real rather than
 * implicit. TRANSFER is what reassign_resource has always done:
 * ownership itself moves, and the old owner loses access entirely.
 * STREAM and COW mean the resource stays put -- owner_node never
 * changes -- and a non-owner accesses it through pagefault.c's
 * mechanism instead: split the covering 2MB region locally, mark the
 * pages absent (STREAM) or read-only (COW), and let the first access
 * fault, resolve, and retry. This property doesn't DO that resolution
 * itself -- it's metadata a node reads via GET_RESOURCE_INFO before
 * setting up its own local fault handling; see coreinit.c's
 * job_stream_demo for the current, only implementation of a
 * non-owner actually doing so. */
#define RESOURCE_SYNC_TRANSFER 0
#define RESOURCE_SYNC_STREAM 1
#define RESOURCE_SYNC_COW 2

/* A resource object's own methods (see cluster_scheduler.c's
 * resource_handler) -- exposed here so any caller holding a resource's
 * handle directly can invoke it without going through cluster_scheduler
 * as a middleman, the same way job_kprintf reserves the serial resource
 * for the duration of one print call. */
#define RESOURCE_METHOD_GET_INFO 0
#define RESOURCE_METHOD_RESERVE 1
#define RESOURCE_METHOD_RELEASE_RESERVATION 2
#define RESOURCE_METHOD_REASSIGN 3

/* Mutex 0 is reserved for the memory allocator -- both
 * handle_allocate_slice and handle_release_slice assume the caller
 * already holds this mutex, and neither does any locking of its own
 * (no spinlock, no busy-retry response -- this is an RTOS, so
 * contention is resolved by blocking the calling JOB, not by spinning
 * or asking the caller to poll). job_memory.c's
 * job_memory_try_acquire_local is the intended, and really only sane,
 * way to reach either method: it does the try_lock, tells the calling
 * job to return JOB_BLOCKED_ON_MUTEX (letting the scheduler run other
 * work on this core in the meantime -- real preemption, not a spin
 * loop) if contended, and unlocks again once the allocation call
 * returns. Ordinary scheduler_mutex_try_lock/unlock callers elsewhere
 * should treat this id as taken and pick a different slot. */
#define ALLOCATOR_MUTEX_ID 0

/* Two genuinely different kinds of sync, not one mechanism papering over
 * both:
 *
 * RESOURCE_MODE_TRANSFER -- ownership changing IS what makes a resource
 *   usable by its new owner; nothing else has to happen. This is what
 *   MEMORY and CPU_CORE resources use today, and it's only that simple
 *   because this project's nodes currently share one physical address
 *   space -- reassigning a memory resource's owner already grants real,
 *   direct access to the same bytes. The day nodes stop sharing memory,
 *   TRANSFER for MEMORY specifically will need to mean "the data was
 *   actually copied," not just a bookkeeping change; that's real,
 *   separate future work, not something quietly assumed solved here.
 *
 * RESOURCE_MODE_STREAM -- the resource's real, usable object stays
 *   exactly where it is (host_node, at backing_capability -- a LOCAL
 *   handle meaningful only on that node's own table), and ownership
 *   changing does NOT grant direct access. A new owner reaches a
 *   streamed resource by querying get_resource_access for
 *   {host_node, backing_capability} and creating its own local PROXY
 *   (see proxy.h) to it -- exactly the mechanism already built for
 *   making a remote capability behave like a local one. This is the
 *   only honest option for anything genuinely tied to one node's own
 *   hardware, like the keyboard IRQ example this whole model grew out
 *   of: the wire doesn't move just because a bookkeeping field changed. */
#define RESOURCE_MODE_TRANSFER 0
#define RESOURCE_MODE_STREAM 1

extern object_schema_t g_cluster_scheduler_schema;

/* Every resource object shares this one schema -- exposed so a node
 * that only DISCOVERED a resource's handle (an AP reaching the shared
 * "serial" resource, say) can build a local proxy to it the same way
 * it already does for cluster_scheduler/mutex_manager. Call
 * resource_object_build_schema() once before using this, same
 * reasoning as cluster_scheduler_build_schema's own doc comment. */
extern object_schema_t g_resource_schema;
void resource_object_build_schema(void);
void cluster_scheduler_handler(void *data, uint32_t method_id,
                               const void *msg, size_t len, void *response,
                               size_t response_cap, size_t *out_response_len);

/* Called once, by the BSP, before this object is created -- needs the
 * memory manager's handle and a LOCAL kernel_invoke, since this object
 * always runs on the BSP and reaches the memory manager the same
 * ordinary way any local code would. */
void cluster_scheduler_init_schema(
    capability_t memory_manager,
    int (*kernel_invoke_fn)(capability_t target, uint32_t method_id,
                           const void *msg, size_t len, void *response,
                           size_t response_cap, size_t *out_response_len),
    capability_t factory);

/* Populates JUST the schema (method ids, param/result shapes) with no
 * other side effects -- what any node needs before creating a LOCAL
 * PROXY to a remote cluster scheduler, since kernel_invoke validates a
 * request against the schema the LOCAL object (the proxy) was created
 * with, not against whatever the real remote object actually does.
 * cluster_scheduler_init_schema calls this internally too; a node that
 * only needs the schema (not the real implementation's state) should
 * call this directly instead. */
void cluster_scheduler_build_schema(void);
