#include "cluster_scheduler.h"
#include "capnp_build.h"
#include "schema.h"
#include "scheduler.h"
#include "serial.h"
#include "try_lock.h"
#include "../include/smp_layout.h"

#define MAX_NODES 16
#define MAX_ALLOC_RECORDS 32
#define MAX_RESOURCES 48
#define MAX_JOBS 32
#define MAX_MUTEXES 8

typedef enum { BLOCK_NONE = 0, BLOCK_MUTEX = 1, BLOCK_JOB = 2 } block_kind_t;

/* Forward declarations: the resource object block below needs these,
 * but their "real" static definitions live further down alongside the
 * rest of cluster_scheduler's own state, in the order that made the
 * most sense for reading the file as a whole. */
static int (*g_kernel_invoke)(capability_t, uint32_t, const void *, size_t,
                              void *, size_t, size_t *);
static capability_t g_factory; /* set once, in cluster_scheduler_init_schema
                                * below -- see coreinit.c's own g_factory
                                * comment for why every file that needs
                                * this keeps its own static copy rather
                                * than sharing one global. */

typedef struct {
    uint64_t node_id;
    uint64_t base;
    uint64_t size;
    uint64_t kind;
    int used;
} alloc_entry_t;

/* A resource is now a real, independent capability object -- created
 * via the factory object's own CREATE, addressed by its own handle, queried and
 * mutated only through kernel_invoke on that handle, never through
 * direct struct access from outside. cluster_scheduler is just another
 * caller of these objects now, including for its OWN internal
 * scheduling logic (resource_ok_for below) -- proximity in the same
 * process doesn't exempt it from going through the same interface
 * everything else uses; that's the whole point of "objects
 * everywhere."
 *
 * owner_node is who currently owns it -- persistent, changeable via
 * REASSIGN, not tied to any job's lifetime (this is the field that
 * makes "the keyboard IRQ can be reassigned" meaningful). reserved_by
 * is which job currently holds it (0 = free) -- transient, released
 * the moment that job finishes or explicitly releases it, the same
 * shape a mutex has. capacity is kind-specific: byte size for MEMORY,
 * unused (0) for CPU_CORE/PERIPHERAL. */
typedef struct {
    uint64_t owner_node;
    uint64_t kind;
    uint64_t descriptor;
    uint64_t capacity;
    uint64_t reserved_by;
    uint64_t sync_kind; /* RESOURCE_SYNC_TRANSFER/STREAM/COW */
} resource_data_t;

/* RESOURCE_METHOD_* constants now live in cluster_scheduler.h. */

static const struct_schema_t res_s1 = {.data_words = 1, .pointer_count = 0, .pointers = NULL};
static const struct_schema_t res_s6 = {.data_words = 6, .pointer_count = 0, .pointers = NULL};

static method_schema_t g_resource_methods[4];
object_schema_t g_resource_schema;

void resource_object_build_schema(void)
{
    g_resource_methods[0].method_id = RESOURCE_METHOD_GET_INFO;
    g_resource_methods[0].param_schema = &res_s1; /* dummy word, unused */
    g_resource_methods[0].result_schema = &res_s6;
    g_resource_methods[1].method_id = RESOURCE_METHOD_RESERVE;
    g_resource_methods[1].param_schema = &res_s1; /* requesting_job */
    g_resource_methods[1].result_schema = &res_s1; /* ok */
    g_resource_methods[2].method_id = RESOURCE_METHOD_RELEASE_RESERVATION;
    g_resource_methods[2].param_schema = &res_s1;
    g_resource_methods[2].result_schema = &res_s1;
    g_resource_methods[3].method_id = RESOURCE_METHOD_REASSIGN;
    g_resource_methods[3].param_schema = &res_s1; /* new_owner_node */
    g_resource_methods[3].result_schema = &res_s1;

    g_resource_schema.methods = g_resource_methods;
    g_resource_schema.method_count = 4;
}

static void resource_handler(void *data, uint32_t method_id, const void *msg,
                             size_t len, void *response, size_t response_cap,
                             size_t *out_response_len)
{
    (void)len;
    resource_data_t *r = (resource_data_t *)data;
    const uint64_t *words = capnp_read_flat(msg);
    *out_response_len = 0;

    switch (method_id) {
    case RESOURCE_METHOD_GET_INFO: {
        uint64_t out[6] = {r->owner_node,  r->kind,      r->descriptor,
                           r->capacity,    r->sync_kind, r->reserved_by};
        *out_response_len = capnp_build_flat(response, response_cap, out, 6);
        break;
    }
    case RESOURCE_METHOD_RESERVE: {
        uint64_t requesting_job = words[0];
        uint64_t ok = 0;
        if (r->reserved_by == requesting_job) {
            ok = 1; /* already held by this same job -- idempotent,
                     * no CAS needed */
        } else if (__sync_bool_compare_and_swap(&r->reserved_by, 0,
                                                requesting_job)) {
            /* Atomic: was 0, is now requesting_job, in one instruction.
             * Without this, two cores concurrently reserving the same
             * resource could both read reserved_by==0 before either
             * wrote it, and both believe they'd succeeded -- a real,
             * latent race, not hypothetical, now the actual mutual
             * exclusion primitive job_kprintf relies on for the serial
             * port resource. Not a spinlock: one atomic op, never
             * retried, never waits. */
            ok = 1;
        }
        *out_response_len = capnp_build_flat(response, response_cap, &ok, 1);
        break;
    }
    case RESOURCE_METHOD_RELEASE_RESERVATION: {
        uint64_t requesting_job = words[0];
        uint64_t ok = __sync_bool_compare_and_swap(&r->reserved_by,
                                                   requesting_job, 0)
                          ? 1
                          : 0;
        *out_response_len = capnp_build_flat(response, response_cap, &ok, 1);
        break;
    }
    case RESOURCE_METHOD_REASSIGN: {
        uint64_t new_owner_node = words[0];
        uint64_t ok = 0;
        if (r->reserved_by == 0) {
            r->owner_node = new_owner_node;
            ok = 1;
        }
        *out_response_len = capnp_build_flat(response, response_cap, &ok, 1);
        break;
    }
    default:
        break;
    }
}

/* Storage for resource objects -- a fixed pool, same honest scoping
 * limitation as coreinit.c's own BOOTSTRAP_RECORD_POOL_SIZE: NOT a
 * reintroduction of the kernel's old capability-table ceiling (that one
 * is genuinely gone, see cap.h), but a separate, smaller, still-real
 * bound on how many resource OBJECTS this particular pool can back.
 * Drawing this from mem_mgr instead, for genuinely unbounded resource
 * creation, needs resource creation to go through the same
 * ALLOCATOR_MUTEX_ID guard job_memory.c already uses -- real, sensible
 * follow-up work, not done here to keep this pass's risk contained to
 * "resources become real objects" alone. */
#define RESOURCE_POOL_SIZE 128
static uint8_t g_resource_storage[RESOURCE_POOL_SIZE]
                                  [CAP_RECORD_SIZE + sizeof(resource_data_t)]
    __attribute__((aligned(CAP_RECORD_ALIGN)));
static size_t g_resource_storage_next;

/* g_resource_storage_next and g_node_storage_next (below) are bump
 * allocators, incremented via __sync_fetch_and_add -- lock-free, one
 * atomic instruction, no contention window at all. This used to be a
 * spinlock-protected increment; that fixed a real, previously latent
 * race (two cores concurrently creating a resource could read the SAME
 * counter value before either incremented it, corrupting a shared
 * storage slot) but did it by making a contending core sit and spin,
 * which is both wasted cycles and a priority-inversion hazard for an
 * RTOS. A single atomic fetch-and-add closes the same race with no
 * waiting at all. g_storage_lock itself remains, narrowed to the
 * handful of genuinely multi-step sections below (pick_next_for_node,
 * the node/job/resource teardown sweeps) that read-decide-and-mutate
 * across more than one location and can't be reduced to a single
 * atomic op -- see trylock_try_acquire's own doc comment in
 * try_lock.h for why those are try-only, never spun on either. */
static trylock_t g_storage_lock;

/* For the handful of genuinely compound operations below (look up a
 * job's data, then go on reading/writing through that pointer) that
 * still need real exclusivity -- see job_data_for_handle_locked's own
 * note on why a stale pointer into a destroyed-and-reused slot is a
 * real bug, not just a stale scheduling hint. This retries via hlt,
 * not a spin: hlt actually stops the core until the next interrupt,
 * rather than burning cycles reading a flag in a loop, and these
 * sections are short and rare enough that "wait for an interrupt, try
 * again" is a perfectly good fit -- there's no job context here to
 * job_exec_wait on instead, this runs as a plain synchronous
 * kernel_invoke handler. */
static void storage_lock_acquire_wait(void)
{
    while (!trylock_try_acquire(&g_storage_lock)) {
        __asm__ volatile("hlt");
    }
}

/* A lightweight discovery index -- NOT the resources' own state (that
 * lives only in each object itself, reachable only via kernel_invoke on
 * its handle), just enough to answer "which resources exist, and what
 * kind is each" for resource_kind-based job matching and leave_node's
 * cleanup sweep. kind is cached here because it's immutable for a
 * resource's whole life; owner_node deliberately is NOT cached (it can
 * change via REASSIGN) -- anything that needs current ownership queries
 * the object itself via GET_INFO instead of trusting a copy that could
 * go stale. */
typedef struct {
    capability_t handle;
    uint64_t kind;
    int used;
} resource_index_entry_t;

#define MAX_RESOURCE_INDEX RESOURCE_POOL_SIZE
static resource_index_entry_t g_resource_index[MAX_RESOURCE_INDEX];

static capability_t create_resource_object(uint64_t owner_node, uint64_t kind,
                                           uint64_t descriptor,
                                           uint64_t capacity,
                                           uint64_t sync_kind,
                                           uint64_t initial_reserved_by)
{
    capability_t none = {0};
    size_t slot_index = __sync_fetch_and_add(&g_resource_storage_next, 1);
    if (slot_index >= RESOURCE_POOL_SIZE) {
        kprintf("[coreinit]   cluster_scheduler: resource pool exhausted\n");
        return none;
    }
    uint8_t *storage = g_resource_storage[slot_index];
    void *record_storage = storage;
    resource_data_t *rd = (resource_data_t *)(storage + CAP_RECORD_SIZE);
    rd->owner_node = owner_node;
    rd->kind = kind;
    rd->descriptor = descriptor;
    rd->capacity = capacity;
    rd->reserved_by = initial_reserved_by;
    rd->sync_kind = sync_kind;

    uint64_t create_words[4] = {(uint64_t)(uintptr_t)resource_handler,
                                (uint64_t)(uintptr_t)rd,
                                (uint64_t)(uintptr_t)&g_resource_schema,
                                (uint64_t)(uintptr_t)record_storage};
    uint8_t create_req[64];
    size_t create_req_len =
        capnp_build_flat(create_req, sizeof(create_req), create_words, 4);
    uint8_t create_resp[24];
    size_t create_resp_len = 0;
    capability_t result = none;
    if (g_kernel_invoke(g_factory, 0, create_req, create_req_len,
                        create_resp, sizeof(create_resp), &create_resp_len) &&
        create_resp_len >= 24) {
        result.handle = capnp_read_flat(create_resp)[0];
    }
    if (result.handle == 0) {
        return none;
    }

    for (int i = 0; i < MAX_RESOURCE_INDEX; i++) {
        if (__sync_bool_compare_and_swap(&g_resource_index[i].used, 0, 1)) {
            g_resource_index[i].handle = result;
            g_resource_index[i].kind = kind;
            break;
        }
    }
    return result;
}

/* Thin helpers over a resource object's own interface, so callers here
 * read like ordinary field access even though every one of these is a
 * real kernel_invoke round trip now. */
static int resource_get_info(capability_t handle, uint64_t *owner_node,
                             uint64_t *kind, uint64_t *descriptor,
                             uint64_t *capacity, uint64_t *sync_kind,
                             uint64_t *reserved_by)
{
    if (handle.handle == 0) return 0;
    uint64_t dummy[1] = {0};
    uint8_t req[24];
    size_t req_len = capnp_build_flat(req, sizeof(req), dummy, 1);
    uint8_t resp[64];
    size_t resp_len = 0;
    if (!g_kernel_invoke(handle, RESOURCE_METHOD_GET_INFO, req, req_len, resp,
                         sizeof(resp), &resp_len) ||
        resp_len < 56) {
        return 0;
    }
    const uint64_t *out = capnp_read_flat(resp);
    if (owner_node) *owner_node = out[0];
    if (kind) *kind = out[1];
    if (descriptor) *descriptor = out[2];
    if (capacity) *capacity = out[3];
    if (sync_kind) *sync_kind = out[4];
    if (reserved_by) *reserved_by = out[5];
    return 1;
}

/* Scans the discovery index for a RESOURCE_KIND_MMU resource owned by
 * node_id -- the one, authoritative answer to "does this node have an
 * MMU" from anywhere else in this file, sourced from what
 * handle_join_node recorded at join time (see arch_mmu.h), not
 * re-derived here. */
static int node_has_mmu(uint64_t node_id)
{
    for (int i = 0; i < MAX_RESOURCE_INDEX; i++) {
        if (!g_resource_index[i].used ||
            g_resource_index[i].kind != RESOURCE_KIND_MMU) {
            continue;
        }
        uint64_t owner;
        if (resource_get_info(g_resource_index[i].handle, &owner, NULL, NULL,
                              NULL, NULL, NULL) &&
            owner == node_id) {
            return 1;
        }
    }
    return 0;
}

static int resource_reserve(capability_t handle, uint64_t requesting_job)
{
    if (handle.handle == 0) return 0;
    uint64_t words[1] = {requesting_job};
    uint8_t req[24];
    size_t req_len = capnp_build_flat(req, sizeof(req), words, 1);
    uint8_t resp[24];
    size_t resp_len = 0;
    if (!g_kernel_invoke(handle, RESOURCE_METHOD_RESERVE, req, req_len, resp,
                         sizeof(resp), &resp_len) ||
        resp_len < 24) {
        return 0;
    }
    return capnp_read_flat(resp)[0] != 0;
}

static int resource_release_reservation(capability_t handle,
                                        uint64_t requesting_job)
{
    if (handle.handle == 0) return 0;
    uint64_t words[1] = {requesting_job};
    uint8_t req[24];
    size_t req_len = capnp_build_flat(req, sizeof(req), words, 1);
    uint8_t resp[24];
    size_t resp_len = 0;
    if (!g_kernel_invoke(handle, RESOURCE_METHOD_RELEASE_RESERVATION, req,
                         req_len, resp, sizeof(resp), &resp_len) ||
        resp_len < 24) {
        return 0;
    }
    return capnp_read_flat(resp)[0] != 0;
}

static int resource_reassign(capability_t handle, uint64_t new_owner_node)
{
    if (handle.handle == 0) return 0;
    uint64_t words[1] = {new_owner_node};
    uint8_t req[24];
    size_t req_len = capnp_build_flat(req, sizeof(req), words, 1);
    uint8_t resp[24];
    size_t resp_len = 0;
    if (!g_kernel_invoke(handle, RESOURCE_METHOD_REASSIGN, req, req_len, resp,
                         sizeof(resp), &resp_len) ||
        resp_len < 24) {
        return 0;
    }
    return capnp_read_flat(resp)[0] != 0;
}

/* job_info_t mirrors cluster_job_t's old fields for callers that read a
 * job's full state in one shot -- but a job's identity and lifecycle no
 * longer live in a struct field at all: "id" is simply the job
 * object's own capability handle (see job_get_info/create_job_object
 * below), and "active" no longer exists as a flag -- a job that has
 * finished is DESTROYED outright, the same lifecycle every other
 * object in this file now has. */
typedef struct {
    job_type_t type;
    uint64_t ctx;
    int base_priority;
    int effective_priority;
    uint64_t node_affinity;
    uint64_t assigned_node;
    uint64_t resource_id;
    uint64_t resource_kind;
    uint64_t resource_min_capacity;
    uint64_t bound_resource_id;
    int blocked;
    block_kind_t block_kind;
    uint64_t block_id;
} job_info_t;

static alloc_entry_t g_allocs[MAX_ALLOC_RECORDS];

static capability_t g_memory_manager;
static capability_t g_pool_resource; /* the one resource representing the
                                      * whole allocatable window -- see
                                      * handle_allocate_slice/
                                      * handle_release_slice for how
                                      * ALLOCATOR_MUTEX_ID (not this
                                      * resource's own reserved_by) is
                                      * what actually guards allocation. */
/* g_kernel_invoke's real (only) storage is the forward declaration near
 * the top of this file, ahead of the resource object block that needs
 * it too. */

#define METHOD_JOIN_NODE 0
#define METHOD_LEAVE_NODE 1
#define METHOD_REQUEST_ALLOCATION 2
#define METHOD_DECLARE_RESOURCE 3
#define METHOD_REASSIGN_RESOURCE 4
#define METHOD_SUBMIT_JOB 5
#define METHOD_GET_NEXT_JOB 6
#define METHOD_REPORT_JOB_RESULT 7
#define METHOD_MUTEX_TRY_LOCK 8
#define METHOD_MUTEX_UNLOCK 9
#define METHOD_GET_RESOURCE_INFO 10
#define METHOD_ALLOCATE_SLICE 11
#define METHOD_RELEASE_SLICE 12

static const struct_schema_t s3 = {.data_words = 3, .pointer_count = 0, .pointers = NULL};
static const struct_schema_t s1 = {.data_words = 1, .pointer_count = 0, .pointers = NULL};
static const struct_schema_t s4 = {.data_words = 4, .pointer_count = 0, .pointers = NULL};
static const struct_schema_t s2 = {.data_words = 2, .pointer_count = 0, .pointers = NULL};
static const struct_schema_t s5 = {.data_words = 5, .pointer_count = 0, .pointers = NULL};
static const struct_schema_t s6 = {.data_words = 6, .pointer_count = 0, .pointers = NULL};
static const struct_schema_t s7 = {.data_words = 7, .pointer_count = 0, .pointers = NULL};

static method_schema_t g_methods[13];
object_schema_t g_cluster_scheduler_schema;

void cluster_scheduler_build_schema(void)
{
    g_methods[0].method_id = METHOD_JOIN_NODE;
    g_methods[0].param_schema = &s4; /* node_id, arch, region_base,
                                      * signal_target -- see node_data_t.
                                      * Join is identity only; a node
                                      * declares its own resources (cpu
                                      * core, mmu if it has one, ...)
                                      * afterward, the same
                                      * declare_resource path everything
                                      * else uses -- see coreinit.c's
                                      * own post-join calls. */
    g_methods[0].result_schema = NULL;
    g_methods[1].method_id = METHOD_LEAVE_NODE;
    g_methods[1].param_schema = &s1;
    g_methods[1].result_schema = NULL;
    g_methods[2].method_id = METHOD_REQUEST_ALLOCATION;
    g_methods[2].param_schema = &s5;
    g_methods[2].result_schema = &s2;
    g_methods[3].method_id = METHOD_DECLARE_RESOURCE;
    g_methods[3].param_schema = &s5;
    g_methods[3].result_schema = &s1;
    g_methods[4].method_id = METHOD_REASSIGN_RESOURCE;
    g_methods[4].param_schema = &s2;
    g_methods[4].result_schema = &s1;
    g_methods[5].method_id = METHOD_SUBMIT_JOB;
    g_methods[5].param_schema = &s7;
    g_methods[5].result_schema = &s1;
    g_methods[6].method_id = METHOD_GET_NEXT_JOB;
    g_methods[6].param_schema = &s1;
    g_methods[6].result_schema = &s4;
    g_methods[7].method_id = METHOD_REPORT_JOB_RESULT;
    g_methods[7].param_schema = &s3;
    g_methods[7].result_schema = NULL;
    g_methods[8].method_id = METHOD_MUTEX_TRY_LOCK;
    g_methods[8].param_schema = &s2;
    g_methods[8].result_schema = &s1;
    g_methods[9].method_id = METHOD_MUTEX_UNLOCK;
    g_methods[9].param_schema = &s2;
    g_methods[9].result_schema = NULL;
    g_methods[10].method_id = METHOD_GET_RESOURCE_INFO;
    g_methods[10].param_schema = &s1;
    g_methods[10].result_schema = &s6;
    g_methods[11].method_id = METHOD_ALLOCATE_SLICE;
    g_methods[11].param_schema = &s5; /* node_id, job_id, size, kind, sync_kind */
    g_methods[11].result_schema = &s3; /* ok, resource_id, base */
    g_methods[12].method_id = METHOD_RELEASE_SLICE;
    g_methods[12].param_schema = &s2; /* resource_id, job_id */
    g_methods[12].result_schema = &s1; /* ok */

    g_cluster_scheduler_schema.methods = g_methods;
    g_cluster_scheduler_schema.method_count = 13;
}

typedef struct {
    uint64_t node_id;
    uint64_t arch;
    uint64_t region_base;
    uint64_t signal_target; /* architecture-opaque, produced by the
                             * joining node's own cross_core_signal_my_
                             * target() and recorded here verbatim --
                             * see cross_core_signal.h. Never
                             * interpreted by this file; only ever
                             * handed back to cross_core_signal_send
                             * when this node needs to be preempted
                             * from elsewhere in the cluster. */
} node_data_t;

#define NODE_METHOD_GET_INFO 0 /* dummy word in -> {node_id, arch, region_base} */

static const struct_schema_t node_s1 = {.data_words = 1, .pointer_count = 0, .pointers = NULL};
static const struct_schema_t node_s3 = {.data_words = 3, .pointer_count = 0, .pointers = NULL};

static method_schema_t g_node_methods[1];
static object_schema_t g_node_schema;

static void node_object_build_schema(void)
{
    g_node_methods[0].method_id = NODE_METHOD_GET_INFO;
    g_node_methods[0].param_schema = &node_s1;
    g_node_methods[0].result_schema = &node_s3;

    g_node_schema.methods = g_node_methods;
    g_node_schema.method_count = 1;
}

static void node_handler(void *data, uint32_t method_id, const void *msg,
                         size_t len, void *response, size_t response_cap,
                         size_t *out_response_len)
{
    (void)method_id;
    (void)msg;
    (void)len;
    node_data_t *n = (node_data_t *)data;
    uint64_t out[3] = {n->node_id, n->arch, n->region_base};
    *out_response_len = capnp_build_flat(response, response_cap, out, 3);
}

/* Nodes are physical cores -- MAX_NODES is a real, hardware-shaped
 * bound (this many cores could plausibly join), not an artificial
 * ceiling like the kernel's old capability table was. */
static uint8_t g_node_storage[MAX_NODES][CAP_RECORD_SIZE + sizeof(node_data_t)]
    __attribute__((aligned(CAP_RECORD_ALIGN)));
static size_t g_node_storage_next;

/* Discovery index: node_id is safe to cache here (unlike a resource's
 * owner_node, a node's own id never changes after creation -- only
 * whether it's currently a member does, reflected by used/destroy). */
typedef struct {
    capability_t handle;
    uint64_t node_id;
    int used;
} node_index_entry_t;

static node_index_entry_t g_node_index[MAX_NODES];

static capability_t create_node_object(uint64_t node_id, uint64_t arch,
                                       uint64_t region_base,
                                       uint64_t signal_target)
{
    capability_t none = {0};
    size_t slot_index = __sync_fetch_and_add(&g_node_storage_next, 1);
    if (slot_index >= MAX_NODES) {
        kprintf("[coreinit]   cluster_scheduler: node pool exhausted\n");
        return none;
    }
    uint8_t *storage = g_node_storage[slot_index];
    node_data_t *nd = (node_data_t *)(storage + CAP_RECORD_SIZE);
    nd->node_id = node_id;
    nd->arch = arch;
    nd->region_base = region_base;
    nd->signal_target = signal_target;

    uint64_t create_words[4] = {(uint64_t)(uintptr_t)node_handler,
                                (uint64_t)(uintptr_t)nd,
                                (uint64_t)(uintptr_t)&g_node_schema,
                                (uint64_t)(uintptr_t)storage};
    uint8_t create_req[64];
    size_t create_req_len =
        capnp_build_flat(create_req, sizeof(create_req), create_words, 4);
    uint8_t create_resp[24];
    size_t create_resp_len = 0;
    capability_t result = none;
    if (g_kernel_invoke(g_factory, 0, create_req, create_req_len,
                        create_resp, sizeof(create_resp), &create_resp_len) &&
        create_resp_len >= 24) {
        result.handle = capnp_read_flat(create_resp)[0];
    }
    if (result.handle == 0) {
        return none;
    }

    for (int i = 0; i < MAX_NODES; i++) {
        if (__sync_bool_compare_and_swap(&g_node_index[i].used, 0, 1)) {
            g_node_index[i].handle = result;
            g_node_index[i].node_id = node_id;
            break;
        }
    }
    return result;
}

/* Looks up the CURRENT member with this node_id -- 0 handle if none.
 * This is the direct replacement for the old find_node()'s array
 * lookup: still an O(MAX_NODES) scan, but now over a lightweight index
 * of handles rather than direct access to a node's own state. */
static capability_t node_find_by_id(uint64_t node_id)
{
    for (int i = 0; i < MAX_NODES; i++) {
        if (g_node_index[i].used && g_node_index[i].node_id == node_id) {
            return g_node_index[i].handle;
        }
    }
    capability_t none = {0};
    return none;
}

static void node_destroy(uint64_t node_id)
{
    for (int i = 0; i < MAX_NODES; i++) {
        if (g_node_index[i].used && g_node_index[i].node_id == node_id) {
            uint64_t destroy_words[1] = {g_node_index[i].handle.handle};
            uint8_t destroy_req[24];
            size_t destroy_req_len = capnp_build_flat(
                destroy_req, sizeof(destroy_req), destroy_words, 1);
            g_kernel_invoke(g_factory, 1 /* DESTROY */, destroy_req,
                           destroy_req_len, NULL, 0, NULL);
            g_node_index[i].used = 0;
            return;
        }
    }
}

typedef struct {
    int locked;
    uint64_t holder_job_id;
} mutex_data_t;

#define MUTEX_METHOD_TRY_LOCK 0 /* {requesting_job_id} -> {ok, holder_job_id} --
                                 * holder_job_id is meaningful when ok=0: who
                                 * currently holds it, so the caller (always
                                 * cluster_scheduler's own forwarding wrapper)
                                 * can do priority inheritance without a
                                 * second round trip. */
#define MUTEX_METHOD_UNLOCK 1 /* {requesting_job_id} -> {ok} */

static const struct_schema_t mutex_s1 = {.data_words = 1, .pointer_count = 0, .pointers = NULL};
static const struct_schema_t mutex_s2 = {.data_words = 2, .pointer_count = 0, .pointers = NULL};

static method_schema_t g_mutex_methods[2];
static object_schema_t g_mutex_schema;

static void mutex_object_build_schema(void)
{
    g_mutex_methods[0].method_id = MUTEX_METHOD_TRY_LOCK;
    g_mutex_methods[0].param_schema = &mutex_s1;
    g_mutex_methods[0].result_schema = &mutex_s2;
    g_mutex_methods[1].method_id = MUTEX_METHOD_UNLOCK;
    g_mutex_methods[1].param_schema = &mutex_s1;
    g_mutex_methods[1].result_schema = &mutex_s1;

    g_mutex_schema.methods = g_mutex_methods;
    g_mutex_schema.method_count = 2;
}

static void mutex_handler(void *data, uint32_t method_id, const void *msg,
                          size_t len, void *response, size_t response_cap,
                          size_t *out_response_len)
{
    (void)len;
    mutex_data_t *m = (mutex_data_t *)data;
    const uint64_t *words = capnp_read_flat(msg);
    *out_response_len = 0;

    if (method_id == MUTEX_METHOD_TRY_LOCK) {
        uint64_t requesting_job_id = words[0];
        uint64_t ok = 0;
        if (!m->locked) {
            m->locked = 1;
            m->holder_job_id = requesting_job_id;
            ok = 1;
        }
        uint64_t out[2] = {ok, m->holder_job_id};
        *out_response_len = capnp_build_flat(response, response_cap, out, 2);
    } else if (method_id == MUTEX_METHOD_UNLOCK) {
        uint64_t requesting_job_id = words[0];
        uint64_t ok = 0;
        if (m->locked && m->holder_job_id == requesting_job_id) {
            m->locked = 0;
            ok = 1;
        }
        *out_response_len = capnp_build_flat(response, response_cap, &ok, 1);
    }
}

/* A small, fixed pool -- MAX_MUTEXES was already a deliberate, small
 * constant in the original design (a well-known set of coordination
 * primitives, ALLOCATOR_MUTEX_ID chief among them), not something
 * meant to grow at runtime the way resources or jobs do. Pre-created
 * eagerly here, one object per integer mutex_id 0..MAX_MUTEXES-1,
 * so every existing caller (job_memory.c's ALLOCATOR_MUTEX_ID=0,
 * scheduler_mutex_try_lock/unlock's own int mutex_id parameter) keeps
 * working completely unchanged -- only the STORAGE backing each mutex
 * becomes a real object; the integer-id interface on top, and
 * cluster_scheduler remaining the one thing a remote node proxies,
 * both stay exactly as they were. */
static uint8_t g_mutex_storage[MAX_MUTEXES]
                               [CAP_RECORD_SIZE + sizeof(mutex_data_t)]
    __attribute__((aligned(CAP_RECORD_ALIGN)));
static capability_t g_mutex_handles[MAX_MUTEXES];

static void create_all_mutex_objects(void)
{
    for (int i = 0; i < MAX_MUTEXES; i++) {
        uint8_t *storage = g_mutex_storage[i];
        mutex_data_t *md = (mutex_data_t *)(storage + CAP_RECORD_SIZE);
        md->locked = 0;
        md->holder_job_id = 0;

        uint64_t create_words[4] = {(uint64_t)(uintptr_t)mutex_handler,
                                    (uint64_t)(uintptr_t)md,
                                    (uint64_t)(uintptr_t)&g_mutex_schema,
                                    (uint64_t)(uintptr_t)storage};
        uint8_t create_req[64];
        size_t create_req_len =
            capnp_build_flat(create_req, sizeof(create_req), create_words, 4);
        uint8_t create_resp[24];
        size_t create_resp_len = 0;
        g_mutex_handles[i].handle = 0;
        if (g_kernel_invoke(g_factory, 0, create_req, create_req_len,
                            create_resp, sizeof(create_resp),
                            &create_resp_len) &&
            create_resp_len >= 24) {
            g_mutex_handles[i].handle = capnp_read_flat(create_resp)[0];
        }
    }
    kprintf("[coreinit]   cluster_scheduler: %u mutex object(s) created "
            "(mutex 0 reserved for the allocator, see ALLOCATOR_MUTEX_ID)\n",
            (unsigned)MAX_MUTEXES);
}

static capability_t mutex_object_for_id(int mutex_id)
{
    capability_t none = {0};
    if (mutex_id < 0 || mutex_id >= MAX_MUTEXES) {
        return none;
    }
    return g_mutex_handles[mutex_id];
}

typedef struct {
    job_type_t type;
    uint64_t ctx;
    int base_priority;
    int effective_priority;
    uint64_t node_affinity;
    uint64_t assigned_node;
    uint64_t resource_id;
    uint64_t resource_kind;
    uint64_t resource_min_capacity;
    uint64_t bound_resource_id;
    int blocked;
    block_kind_t block_kind;
    uint64_t block_id;
    volatile int dispatched; /* CAS-gated exclusive claim, set the
                              * instant pick_next_for_node hands this
                              * job to a node, cleared when that node
                              * reports back (see
                              * handle_report_job_result). Distinct from
                              * "blocked": blocked is false BOTH while a
                              * job is waiting to be picked AND while
                              * it's actively running elsewhere, so it
                              * alone can't stop two nodes from both
                              * picking the same idle job at once --
                              * this field is what actually prevents
                              * that double-dispatch, without a lock. */
} job_data_t;

#define JOB_METHOD_GET_INFO 0 /* dummy word in -> 13-word job_info_t, in
                               * field order */
#define JOB_METHOD_UPDATE_SCHEDULING_STATE 1 /* {assigned_node,
                                              * bound_resource_id, blocked,
                                              * block_kind, block_id,
                                              * effective_priority} -> {ok}
                                              * -- one bulk write rather
                                              * than a field setter per
                                              * mutation: the scheduling
                                              * loop below is the hottest
                                              * path in the whole system,
                                              * and a real, unbounded
                                              * number of individual
                                              * kernel_invoke round trips
                                              * per job per tick was not
                                              * an acceptable cost the way
                                              * it was for resources
                                              * (created and queried far
                                              * less often). */

static const struct_schema_t job_s1 = {.data_words = 1, .pointer_count = 0, .pointers = NULL};
static const struct_schema_t job_s6 = {.data_words = 6, .pointer_count = 0, .pointers = NULL};
static const struct_schema_t job_s13 = {.data_words = 13, .pointer_count = 0, .pointers = NULL};

static method_schema_t g_job_methods[2];
static object_schema_t g_job_schema;

static void job_object_build_schema(void)
{
    g_job_methods[0].method_id = JOB_METHOD_GET_INFO;
    g_job_methods[0].param_schema = &job_s1;
    g_job_methods[0].result_schema = &job_s13;
    g_job_methods[1].method_id = JOB_METHOD_UPDATE_SCHEDULING_STATE;
    g_job_methods[1].param_schema = &job_s6;
    g_job_methods[1].result_schema = &job_s1;

    g_job_schema.methods = g_job_methods;
    g_job_schema.method_count = 2;
}

static void job_handler(void *data, uint32_t method_id, const void *msg,
                        size_t len, void *response, size_t response_cap,
                        size_t *out_response_len)
{
    (void)len;
    job_data_t *j = (job_data_t *)data;
    const uint64_t *words = capnp_read_flat(msg);
    *out_response_len = 0;

    if (method_id == JOB_METHOD_GET_INFO) {
        uint64_t out[13] = {(uint64_t)j->type,
                            j->ctx,
                            (uint64_t)(int64_t)j->base_priority,
                            (uint64_t)(int64_t)j->effective_priority,
                            j->node_affinity,
                            j->assigned_node,
                            j->resource_id,
                            j->resource_kind,
                            j->resource_min_capacity,
                            j->bound_resource_id,
                            (uint64_t)j->blocked,
                            (uint64_t)j->block_kind,
                            j->block_id};
        *out_response_len = capnp_build_flat(response, response_cap, out, 13);
    } else if (method_id == JOB_METHOD_UPDATE_SCHEDULING_STATE) {
        j->assigned_node = words[0];
        j->bound_resource_id = words[1];
        j->blocked = (int)words[2];
        j->block_kind = (block_kind_t)words[3];
        j->block_id = words[4];
        j->effective_priority = (int)words[5];
        uint64_t ok = 1;
        *out_response_len = capnp_build_flat(response, response_cap, &ok, 1);
    }
}

/* Jobs churn -- created and finished constantly, unlike resources,
 * nodes, or mutexes -- but that no longer means a separate free-list
 * stack: same scan-and-CAS-a-free-slot pattern as node/resource
 * allocation above, just re-run more often. A job that finishes is
 * destroyed outright (see handle_report_job_result), and its slot
 * becomes available again for the very next submission. MAX_JOBS stays
 * the pool size, same bound the original array already had. */
#define JOB_POOL_SIZE MAX_JOBS
static uint8_t g_job_storage[JOB_POOL_SIZE][CAP_RECORD_SIZE + sizeof(job_data_t)]
    __attribute__((aligned(CAP_RECORD_ALIGN)));
static volatile int g_job_storage_used[JOB_POOL_SIZE];
static capability_t g_job_handles[JOB_POOL_SIZE];

static capability_t create_job_object(job_type_t type, uint64_t ctx,
                                      int priority, uint64_t node_affinity,
                                      uint64_t resource_id,
                                      uint64_t resource_kind,
                                      uint64_t resource_min_capacity)
{
    capability_t none = {0};
    size_t slot = JOB_POOL_SIZE;
    for (size_t i = 0; i < JOB_POOL_SIZE; i++) {
        if (__sync_bool_compare_and_swap(&g_job_storage_used[i], 0, 1)) {
            slot = i;
            break;
        }
    }
    if (slot == JOB_POOL_SIZE) {
        kprintf("[coreinit]   cluster_scheduler: job pool exhausted\n");
        return none;
    }

    uint8_t *storage = g_job_storage[slot];
    job_data_t *jd = (job_data_t *)(storage + CAP_RECORD_SIZE);
    jd->type = type;
    jd->ctx = ctx;
    jd->base_priority = priority;
    jd->effective_priority = priority;
    jd->node_affinity = node_affinity;
    jd->assigned_node = NODE_AFFINITY_ANY;
    jd->resource_id = resource_id;
    jd->resource_kind = resource_kind;
    jd->resource_min_capacity = resource_min_capacity;
    jd->bound_resource_id = 0;
    jd->blocked = 0;
    jd->block_kind = BLOCK_NONE;
    jd->block_id = 0;
    jd->dispatched = 0;

    uint64_t create_words[4] = {(uint64_t)(uintptr_t)job_handler,
                                (uint64_t)(uintptr_t)jd,
                                (uint64_t)(uintptr_t)&g_job_schema,
                                (uint64_t)(uintptr_t)storage};
    uint8_t create_req[64];
    size_t create_req_len =
        capnp_build_flat(create_req, sizeof(create_req), create_words, 4);
    uint8_t create_resp[24];
    size_t create_resp_len = 0;
    capability_t result = none;
    if (g_kernel_invoke(g_factory, 0, create_req, create_req_len,
                        create_resp, sizeof(create_resp), &create_resp_len) &&
        create_resp_len >= 24) {
        result.handle = capnp_read_flat(create_resp)[0];
    }

    if (result.handle == 0) {
        /* We alone hold this slot (we won its claiming CAS and never
         * published its handle to anyone), so releasing it back is a
         * plain store, no CAS needed. */
        g_job_storage_used[slot] = 0;
    } else {
        g_job_handles[slot] = result;
    }
    return result;
}

static void destroy_job_object(capability_t job)
{
    for (size_t i = 0; i < JOB_POOL_SIZE; i++) {
        /* Cheap pre-filter -- may be stale, that's fine, it's not the
         * real gate. Capability handles are monotonically allocated
         * and never reused (the factory object's own invariant -- see cap.c), so
         * g_job_handles[i] cannot change to a DIFFERENT job's handle
         * while used stays 1; it can only change after a transition to
         * 0. That means: if our own CAS below succeeds, it was
         * impossible for anyone to have reused this slot in between,
         * so the handle we matched against is still guaranteed correct
         * at the instant we win -- no separate lock or generation
         * counter needed to avoid an ABA-style mix-up here. */
        if (!g_job_storage_used[i] || g_job_handles[i].handle != job.handle) {
            continue;
        }
        if (!__sync_bool_compare_and_swap(&g_job_storage_used[i], 1, 0)) {
            continue; /* someone else already won this destroy (or the
                       * slot changed hands) -- move on rather than
                       * double-free */
        }

        uint64_t destroy_words[1] = {job.handle};
        uint8_t destroy_req[24];
        size_t destroy_req_len =
            capnp_build_flat(destroy_req, sizeof(destroy_req), destroy_words, 1);
        g_kernel_invoke(g_factory, 1 /* DESTROY */, destroy_req,
                       destroy_req_len, NULL, 0, NULL);
        return;
    }
}

/* Note: this file no longer calls this helper internally -- every
 * mutation of a job's own scheduling state now goes through direct
 * access (job_data_for_handle below), since cluster_scheduler owns
 * that storage. JOB_METHOD_UPDATE_SCHEDULING_STATE itself still exists
 * and is reachable via kernel_invoke, for any genuinely external
 * caller (outside this file) that only holds a job's handle -- this
 * particular convenience C wrapper just doesn't have one yet. */

/* Direct access to a job's own storage, for cluster_scheduler's OWN
 * internal scheduling loop only -- NOT a general shortcut. The job's
 * identity, its GET_INFO/UPDATE_SCHEDULING_STATE interface via
 * kernel_invoke, and its create/destroy lifecycle are all real and
 * unchanged: any OTHER caller in the system still only ever reaches a
 * job through its capability handle. This exists because
 * cluster_scheduler is not "another caller" of a job object the way
 * job_memory.c or a mutex object's holder lookup is -- it's the thing
 * that created this storage and already holds the pointer from doing
 * so. Round-tripping through kernel_invoke to read memory the very
 * same function already owns was real, measured overhead on the
 * hottest path in the system (the scheduling loop, invoked
 * continuously by every core), not a meaningful safety or architecture
 * boundary -- a job object was never "communicating with a different
 * system" from cluster_scheduler's own perspective; it's the same
 * component's own state, exposed as an object for everyone ELSE.
 *
 * CALLER MUST HOLD g_storage_lock for this call AND for as long as it
 * goes on reading or writing through the returned pointer. Found this
 * out the hard way: the lookup alone isn't enough to protect against a
 * genuinely concurrent core finishing and destroying the very job a
 * caller is still mid-read on -- a destroyed slot goes straight back
 * onto the free list and can be handed to a brand new, unrelated job
 * while the first caller is still holding what it thinks is a stable
 * pointer into it. Locking only the lookup (as an earlier version of
 * this function did) closes the lookup's own race but not that one. */
static job_data_t *job_data_for_handle_locked(capability_t job)
{
    if (job.handle == 0) return NULL;
    for (size_t i = 0; i < JOB_POOL_SIZE; i++) {
        if (g_job_storage_used[i] && g_job_handles[i].handle == job.handle) {
            return (job_data_t *)(g_job_storage[i] + CAP_RECORD_SIZE);
        }
    }
    return NULL;
}

void cluster_scheduler_init_schema(
    capability_t memory_manager,
    int (*kernel_invoke_fn)(capability_t, uint32_t, const void *, size_t,
                           void *, size_t, size_t *),
    capability_t factory)
{
    g_memory_manager = memory_manager;
    g_kernel_invoke = kernel_invoke_fn;
    g_factory = factory;
    trylock_init(&g_storage_lock);

    for (int i = 0; i < MAX_NODES; i++) g_node_index[i].used = 0;
    for (int i = 0; i < MAX_ALLOC_RECORDS; i++) g_allocs[i].used = 0;
    for (int i = 0; i < MAX_RESOURCE_INDEX; i++) g_resource_index[i].used = 0;
    for (int i = 0; i < JOB_POOL_SIZE; i++) {
        g_job_storage_used[i] = 0;
    }
    g_resource_storage_next = 0;
    g_node_storage_next = 0;

    cluster_scheduler_build_schema();
    resource_object_build_schema();
    node_object_build_schema();
    mutex_object_build_schema();
    job_object_build_schema();
    create_all_mutex_objects();

    /* The pool resource: the whole allocatable window (see
     * smp_layout.h's ALLOC_MIN/ALLOC_MAX, which mem_mgr.c's own
     * alloc/free also use), declared once so it's queryable via
     * GET_RESOURCE_INFO the same as any other resource -- "the
     * allocator owns the entire physical memory resource," made
     * literal. Owned by node 0 (the BSP), since mem_mgr itself only
     * ever runs there. Its handle is purely informational, not a
     * dependency for allocate/release -- those are guarded by
     * ALLOCATOR_MUTEX_ID, not by this resource's own reserved_by. */
    g_pool_resource = create_resource_object(0, RESOURCE_KIND_MEMORY,
                                             ALLOC_MIN, ALLOC_MAX - ALLOC_MIN,
                                             RESOURCE_SYNC_TRANSFER, 0);
    kprintf("[coreinit]   cluster_scheduler: pool resource 0x%X declared, "
            "0x%X-0x%X\n",
            (unsigned)g_pool_resource.handle, (unsigned)ALLOC_MIN,
            (unsigned)ALLOC_MAX);
}

static void release_job_resource(capability_t job, uint64_t bound_resource_id)
{
    capability_t bound = {bound_resource_id};
    resource_release_reservation(bound, job.handle);
}

static void handle_join_node(const uint64_t *words, size_t *out_len)
{
    *out_len = 0;
    uint64_t node_id = words[0], arch = words[1], region_base = words[2];
    uint64_t signal_target = words[3];

    if (node_find_by_id(node_id).handle != 0) {
        kprintf("[coreinit]   cluster_scheduler: node %u already joined, "
                "ignoring\n",
                (unsigned)node_id);
        return;
    }
    capability_t node_obj =
        create_node_object(node_id, arch, region_base, signal_target);
    if (node_obj.handle == 0) {
        kprintf("[coreinit]   cluster_scheduler: join failed, node pool "
                "full\n");
        return;
    }
    kprintf("[coreinit]   cluster_scheduler: node %u joined (node object "
            "0x%X), arch %u, region_base 0x%X, signal_target %u -- "
            "resources declared separately, see declare_resource\n",
            (unsigned)node_id, (unsigned)node_obj.handle, (unsigned)arch,
            (unsigned)region_base, (unsigned)signal_target);
}

static void handle_leave_node(const uint64_t *words, size_t *out_len)
{
    *out_len = 0;
    uint64_t node_id = words[0];

    if (node_find_by_id(node_id).handle == 0) {
        kprintf("[coreinit]   cluster_scheduler: leave_node for unknown "
                "node %u, ignoring\n",
                (unsigned)node_id);
        return;
    }
    node_destroy(node_id);

    /* Sweep the discovery index, querying each resource's CURRENT owner
     * fresh (never cached -- see g_resource_index's own note) so a
     * resource reassigned away from this node beforehand is correctly
     * left alone. */
    for (int i = 0; i < MAX_RESOURCE_INDEX; i++) {
        if (!g_resource_index[i].used) continue;
        uint64_t owner;
        if (resource_get_info(g_resource_index[i].handle, &owner, NULL, NULL,
                              NULL, NULL, NULL) &&
            owner == node_id) {
            uint64_t destroy_words[1] = {g_resource_index[i].handle.handle};
            uint8_t destroy_req[24];
            size_t destroy_req_len = capnp_build_flat(
                destroy_req, sizeof(destroy_req), destroy_words, 1);
            g_kernel_invoke(g_factory, 1 /* DESTROY */, destroy_req,
                           destroy_req_len, NULL, 0, NULL);
            g_resource_index[i].used = 0;
        }
    }

    int cancelled = 0, released = 0;
    for (int i = 0; i < JOB_POOL_SIZE; i++) {
        /* A plain read of a single aligned word -- g_job_storage_used
         * itself is only ever mutated by the CAS-gated claim/destroy
         * pair above, so this can't observe a torn value, only
         * possibly-stale 0/1. Worst case here is skipping a job that
         * just got created (fine, nothing to sweep yet) or looking at
         * one that's just about to be destroyed by someone else (the
         * job_data_t reads below are then stale too, but assigned_node
         * is a scheduling HINT, not a safety-critical invariant -- a
         * job whose destroy is already in flight elsewhere getting one
         * extra harmless write here corrects itself on the very next
         * scheduling pass either way). */
        if (!g_job_storage_used[i]) {
            continue;
        }
        capability_t job = g_job_handles[i];
        job_data_t *j = (job_data_t *)(g_job_storage[i] + CAP_RECORD_SIZE);

        int touches_this_node =
            (j->assigned_node == node_id) ||
            (j->assigned_node == NODE_AFFINITY_ANY &&
             j->node_affinity == node_id);
        if (!touches_this_node) {
            continue;
        }

        if (j->node_affinity == node_id) {
            uint64_t bound_resource_id = j->bound_resource_id;
            release_job_resource(job, bound_resource_id);
            destroy_job_object(job);
            cancelled++;
        } else {
            j->assigned_node = NODE_AFFINITY_ANY;
            released++;
        }
    }

    kprintf("[coreinit]   cluster_scheduler: node %u left -- %d job(s) "
            "cancelled, %d released back to the pool, its own owned "
            "resources removed\n",
            (unsigned)node_id, cancelled, released);
}

static void handle_request_allocation(const uint64_t *words, void *response,
                                      size_t response_cap, size_t *out_len)
{
    uint64_t node_id = words[0], base = words[1], size = words[2],
            kind = words[3], sync_kind = words[4];

    uint64_t claim_words[3] = {base, size, kind};
    uint8_t claim_req[48];
    size_t claim_req_len = capnp_build_flat(claim_req, sizeof(claim_req), claim_words, 3);
    uint8_t claim_resp[24];
    size_t claim_resp_len = 0;
    int ok = 0;
    if (g_kernel_invoke(g_memory_manager, 0, claim_req, claim_req_len,
                        claim_resp, sizeof(claim_resp), &claim_resp_len) &&
        claim_resp_len >= 24) {
        ok = capnp_read_flat(claim_resp)[0] != 0;
    }

    capability_t resource = {0};
    if (ok) {
        for (int i = 0; i < MAX_ALLOC_RECORDS; i++) {
            if (!g_allocs[i].used) {
                g_allocs[i].used = 1;
                g_allocs[i].node_id = node_id;
                g_allocs[i].base = base;
                g_allocs[i].size = size;
                g_allocs[i].kind = kind;
                break;
            }
        }
        resource = create_resource_object(node_id, RESOURCE_KIND_MEMORY, base,
                                          size, sync_kind, 0);
        kprintf("[coreinit]   cluster_scheduler: recorded allocation for "
                "node %u: 0x%X-0x%X, resource 0x%X (capacity %u, sync_kind "
                "%u)\n",
                (unsigned)node_id, (unsigned)base, (unsigned)(base + size),
                (unsigned)resource.handle, (unsigned)size,
                (unsigned)sync_kind);
    }

    uint64_t out[2] = {ok ? 1u : 0u, resource.handle};
    *out_len = capnp_build_flat(response, response_cap, out, 2);
}

/* Must only be called while the caller already holds
 * ALLOCATOR_MUTEX_ID -- see cluster_scheduler.h's own note on that
 * mutex. No locking happens here; this function trusts the caller.
 *
 * The acquiring job (requesting_job_id, not just its node) becomes the
 * new resource's reserved_by immediately, atomically with creation --
 * not left at 0 (unreserved). Left at 0, the resource would be visible
 * to any OTHER job's resource_kind==MEMORY matching the instant it's
 * created, and the scheduler could bind a completely unrelated job to
 * it before the job that actually just allocated it ever touches it.
 * Slice resources are owned by the job that acquired them, not merely
 * attributed to the node it happened to run on -- same as any other
 * job-reserved resource, just reserved by construction here instead of
 * by a later successful resource_kind match. */
static void handle_allocate_slice(const uint64_t *words, void *response,
                                  size_t response_cap, size_t *out_len)
{
    uint64_t node_id = words[0], requesting_job_id = words[1],
            size = words[2], kind = words[3], sync_kind = words[4];

    /* mem_mgr's own alloc (method 1) -- the actual allocator, finding
     * any free gap of the right size, rather than claim (method 0)
     * validating a caller-supplied address. This is the point of this
     * whole method existing separately from request_allocation: a job
     * asking for a slice doesn't choose or even see a physical address
     * until this call returns one. */
    uint64_t alloc_words[2] = {size, kind};
    uint8_t alloc_req[40];
    size_t alloc_req_len = capnp_build_flat(alloc_req, sizeof(alloc_req),
                                            alloc_words, 2);
    uint8_t alloc_resp[40];
    size_t alloc_resp_len = 0;
    int ok = 0;
    uint64_t base = 0;
    if (g_kernel_invoke(g_memory_manager, 1 /* METHOD_ALLOC */, alloc_req,
                        alloc_req_len, alloc_resp, sizeof(alloc_resp),
                        &alloc_resp_len) &&
        alloc_resp_len >= 24) {
        const uint64_t *out = capnp_read_flat(alloc_resp);
        ok = out[0] != 0;
        base = out[1];
    }

    capability_t resource = {0};
    if (ok) {
        resource = create_resource_object(node_id, RESOURCE_KIND_MEMORY,
                                          base, size, sync_kind,
                                          requesting_job_id);
        kprintf("[coreinit]   cluster_scheduler: allocated slice for node "
                "%u, job %u: 0x%X-0x%X, resource 0x%X (sync_kind %u)\n",
                (unsigned)node_id, (unsigned)requesting_job_id,
                (unsigned)base, (unsigned)(base + size),
                (unsigned)resource.handle, (unsigned)sync_kind);
    } else {
        kprintf("[coreinit]   cluster_scheduler: allocate_slice failed, "
                "no free gap of size %u\n",
                (unsigned)size);
    }

    uint64_t out[3] = {ok ? 1u : 0u, resource.handle, base};
    *out_len = capnp_build_flat(response, response_cap, out, 3);
}

/* Must only be called while the caller already holds
 * ALLOCATOR_MUTEX_ID. Also checks that requesting_job_id matches the
 * resource's own reserved_by -- only the job that acquired a slice (or
 * one that later reserved it some other way) can release it, the same
 * ownership check handle_mutex_unlock already applies to its own
 * holder_job_id. */
static void handle_release_slice(const uint64_t *words, void *response,
                                 size_t response_cap, size_t *out_len)
{
    capability_t resource = {words[0]};
    uint64_t requesting_job_id = words[1];
    uint64_t ok = 0;

    uint64_t kind, descriptor, reserved_by;
    if (!resource_get_info(resource, NULL, &kind, &descriptor, NULL, NULL,
                           &reserved_by)) {
        kprintf("[coreinit]   cluster_scheduler: release refused, "
                "resource 0x%X unknown\n",
                (unsigned)resource.handle);
    } else if (kind != RESOURCE_KIND_MEMORY) {
        kprintf("[coreinit]   cluster_scheduler: release refused, "
                "resource 0x%X isn't a memory slice (kind %u)\n",
                (unsigned)resource.handle, (unsigned)kind);
    } else if (reserved_by != requesting_job_id) {
        kprintf("[coreinit]   cluster_scheduler: release refused, "
                "resource 0x%X is owned by job %u, not job %u\n",
                (unsigned)resource.handle, (unsigned)reserved_by,
                (unsigned)requesting_job_id);
    } else {
        uint64_t free_words[1] = {descriptor};
        uint8_t free_req[24];
        size_t free_req_len =
            capnp_build_flat(free_req, sizeof(free_req), free_words, 1);
        uint8_t free_resp[24];
        size_t free_resp_len = 0;
        if (g_kernel_invoke(g_memory_manager, 2 /* METHOD_FREE */, free_req,
                            free_req_len, free_resp, sizeof(free_resp),
                            &free_resp_len) &&
            free_resp_len >= 24 && capnp_read_flat(free_resp)[0] != 0) {
            kprintf("[coreinit]   cluster_scheduler: released resource "
                    "0x%X (job %u), freed 0x%X back to mem_mgr\n",
                    (unsigned)resource.handle, (unsigned)requesting_job_id,
                    (unsigned)descriptor);
            uint64_t destroy_words[1] = {resource.handle};
            uint8_t destroy_req[24];
            size_t destroy_req_len = capnp_build_flat(
                destroy_req, sizeof(destroy_req), destroy_words, 1);
            g_kernel_invoke(g_factory, 1 /* DESTROY */, destroy_req,
                           destroy_req_len, NULL, 0, NULL);
            for (int i = 0; i < MAX_RESOURCE_INDEX; i++) {
                if (g_resource_index[i].used &&
                    g_resource_index[i].handle.handle == resource.handle) {
                    g_resource_index[i].used = 0;
                    break;
                }
            }
            ok = 1;
        } else {
            kprintf("[coreinit]   cluster_scheduler: release refused, "
                    "mem_mgr rejected freeing 0x%X\n",
                    (unsigned)descriptor);
        }
    }
    *out_len = capnp_build_flat(response, response_cap, &ok, 1);
}

static void handle_declare_resource(const uint64_t *words, void *response,
                                    size_t response_cap, size_t *out_len)
{
    uint64_t node_id = words[0], kind = words[1], descriptor = words[2],
            capacity = words[3], sync_kind = words[4];

    if ((sync_kind == RESOURCE_SYNC_STREAM || sync_kind == RESOURCE_SYNC_COW) &&
        !node_has_mmu(node_id)) {
        /* STREAM and COW exist entirely because pagefault.c can split a
         * region and catch faults locally -- a node with no MMU has no
         * such mechanism to offer, so declaring a resource that
         * promises one would be a lie the moment anyone tried to use
         * it. Refuse at declaration time, not on first access. */
        kprintf("[coreinit]   cluster_scheduler: node %u declare REFUSED, "
                "sync_kind %u requires an MMU this node doesn't have\n",
                (unsigned)node_id, (unsigned)sync_kind);
        uint64_t out[1] = {0};
        *out_len = capnp_build_flat(response, response_cap, out, 1);
        return;
    }

    capability_t resource = create_resource_object(node_id, kind, descriptor,
                                                    capacity, sync_kind, 0);
    kprintf("[coreinit]   cluster_scheduler: node %u declared resource "
            "0x%X (kind %u, capacity %u, sync_kind %u)\n",
            (unsigned)node_id, (unsigned)resource.handle, (unsigned)kind,
            (unsigned)capacity, (unsigned)sync_kind);
    uint64_t out[1] = {resource.handle};
    *out_len = capnp_build_flat(response, response_cap, out, 1);
}

static void handle_get_resource_info(const uint64_t *words, void *response,
                                     size_t response_cap, size_t *out_len)
{
    capability_t resource = {words[0]};
    uint64_t owner_node, kind, descriptor, capacity, sync_kind;
    uint64_t out[6];
    if (!resource_get_info(resource, &owner_node, &kind, &descriptor,
                           &capacity, &sync_kind, NULL)) {
        out[0] = 0; /* found = 0 */
        out[1] = out[2] = out[3] = out[4] = out[5] = 0;
    } else {
        out[0] = 1; /* found = 1 */
        out[1] = owner_node;
        out[2] = kind;
        out[3] = descriptor;
        out[4] = capacity;
        out[5] = sync_kind;
    }
    *out_len = capnp_build_flat(response, response_cap, out, 6);
}

static void handle_reassign_resource(const uint64_t *words, void *response,
                                     size_t response_cap, size_t *out_len)
{
    capability_t resource = {words[0]};
    uint64_t new_owner_node = words[1];
    uint64_t ok = 0;

    uint64_t owner_node, reserved_by;
    if (!resource_get_info(resource, &owner_node, NULL, NULL, NULL, NULL,
                           &reserved_by)) {
        kprintf("[coreinit]   cluster_scheduler: reassign refused, "
                "resource 0x%X unknown\n",
                (unsigned)resource.handle);
    } else if (reserved_by != 0) {
        kprintf("[coreinit]   cluster_scheduler: reassign refused, "
                "resource 0x%X is currently reserved by job %u\n",
                (unsigned)resource.handle, (unsigned)reserved_by);
    } else if (node_find_by_id(new_owner_node).handle == 0) {
        kprintf("[coreinit]   cluster_scheduler: reassign refused, node "
                "%u is not currently joined\n",
                (unsigned)new_owner_node);
    } else if (resource_reassign(resource, new_owner_node)) {
        kprintf("[coreinit]   cluster_scheduler: resource 0x%X reassigned "
                "from node %u to node %u\n",
                (unsigned)resource.handle, (unsigned)owner_node,
                (unsigned)new_owner_node);
        ok = 1;
    } else {
        kprintf("[coreinit]   cluster_scheduler: reassign refused, "
                "resource 0x%X's own REASSIGN method rejected it\n",
                (unsigned)resource.handle);
    }
    *out_len = capnp_build_flat(response, response_cap, &ok, 1);
}

static void handle_submit_job(const uint64_t *words, void *response,
                              size_t response_cap, size_t *out_len)
{
    uint64_t node_affinity = words[0], job_type = words[1], ctx = words[2];
    int priority = (int)words[3];
    uint64_t resource_id = words[4], resource_kind = words[5],
            resource_min_capacity = words[6];

    capability_t job = create_job_object((job_type_t)job_type, ctx, priority,
                                         node_affinity, resource_id,
                                         resource_kind, resource_min_capacity);
    if (job.handle == 0) {
        kprintf("[coreinit]   cluster_scheduler: submit_job failed, job "
                "pool full\n");
    }
    uint64_t job_id = job.handle;
    *out_len = capnp_build_flat(response, response_cap, &job_id, 1);
}

/* Returns 1 if j's resource requirement (if any) is currently
 * satisfiable, binding it if this is the first time. A job with no
 * requirement at all is always satisfied. See cluster_scheduler.h for
 * why a kind-based match, once bound, never changes. */
/* Returns 1 if info's resource requirement (if any) is currently
 * satisfiable. A job with no requirement at all is always satisfied.
 * See cluster_scheduler.h for why a kind-based match, once bound, never
 * changes. Does not itself bind anything -- see bind_resource_for. */
static int resource_ok_for(capability_t job, const job_data_t *j)
{
    if (j->bound_resource_id != 0) {
        capability_t bound = {j->bound_resource_id};
        uint64_t reserved_by;
        return resource_get_info(bound, NULL, NULL, NULL, NULL, NULL,
                                 &reserved_by) &&
              (reserved_by == 0 || reserved_by == job.handle);
    }

    if (j->resource_id == 0 && j->resource_kind == RESOURCE_KIND_NONE) {
        return 1; /* no requirement */
    }

    if (j->resource_id != 0) {
        capability_t r = {j->resource_id};
        uint64_t reserved_by;
        return resource_get_info(r, NULL, NULL, NULL, NULL, NULL,
                                 &reserved_by) &&
              reserved_by == 0;
    }

    /* Kind-based: scan the discovery index for a free, capable
     * candidate -- "prefer the most recently declared" now means
     * "prefer the last matching entry found while scanning", since
     * resource identity is a real capability handle (an address) now,
     * not a small monotonic integer safe to compare numerically. */
    for (int i = 0; i < MAX_RESOURCE_INDEX; i++) {
        if (!g_resource_index[i].used ||
            g_resource_index[i].kind != j->resource_kind) {
            continue;
        }
        uint64_t reserved_by, capacity;
        if (resource_get_info(g_resource_index[i].handle, NULL, NULL, NULL,
                              &capacity, NULL, &reserved_by) &&
            reserved_by == 0 && capacity >= j->resource_min_capacity) {
            return 1;
        }
    }
    return 0;
}

/* If j doesn't already have a bound resource, finds, reserves, and
 * writes one directly into j. resource_reserve is still a real
 * kernel_invoke -- the resource being reserved is a genuinely separate
 * object, unlike the job whose own storage this function already
 * holds a pointer to. */
static void bind_resource_for(capability_t job, job_data_t *j)
{
    if (j->bound_resource_id != 0) {
        return; /* already bound, sticky */
    }
    capability_t chosen = {0};
    if (j->resource_id != 0) {
        chosen.handle = j->resource_id;
    } else if (j->resource_kind != RESOURCE_KIND_NONE) {
        for (int i = 0; i < MAX_RESOURCE_INDEX; i++) {
            if (!g_resource_index[i].used ||
                g_resource_index[i].kind != j->resource_kind) {
                continue;
            }
            uint64_t reserved_by, capacity;
            if (resource_get_info(g_resource_index[i].handle, NULL, NULL,
                                  NULL, &capacity, NULL, &reserved_by) &&
                reserved_by == 0 && capacity >= j->resource_min_capacity) {
                chosen = g_resource_index[i].handle;
            }
        }
    }
    if (chosen.handle != 0 && resource_reserve(chosen, job.handle)) {
        j->bound_resource_id = chosen.handle;
    }
}

static void pick_next_for_node(uint64_t requesting_node_id,
                               capability_t *out_job, job_data_t **out_j)
{
    capability_t best_ready = {0}, best_blocked_mutex = {0};
    job_data_t *best_ready_j = NULL, *best_blocked_mutex_j = NULL;

    /* Lock-free scan -- tolerates reading a slightly stale snapshot
     * (another core's concurrent write might not be visible to THIS
     * read yet), which only risks picking a marginally-non-optimal
     * candidate, never anything unsafe: the one thing that actually
     * needs to be exclusive -- not handing the SAME job to two nodes
     * at once -- is enforced below by the CAS on dispatched, not by
     * this scan. */
    for (int i = 0; i < JOB_POOL_SIZE; i++) {
        if (!g_job_storage_used[i]) continue;
        capability_t job = g_job_handles[i];
        job_data_t *j = (job_data_t *)(g_job_storage[i] + CAP_RECORD_SIZE);

        if (j->dispatched) continue; /* already claimed by someone else
                                      * -- see job_data_t's own note */

        int eligible = (j->assigned_node == requesting_node_id) ||
                      (j->assigned_node == NODE_AFFINITY_ANY &&
                       (j->node_affinity == requesting_node_id ||
                        j->node_affinity == NODE_AFFINITY_ANY));
        if (!eligible) continue;
        if (!resource_ok_for(job, j)) continue; /* like being blocked, but
                                                 * not even offered as a
                                                 * mutex-style retry
                                                 * fallback -- no priority
                                                 * inheritance story for a
                                                 * busy resource yet */

        if (j->blocked && j->block_kind == BLOCK_JOB &&
            !job_data_for_handle_locked((capability_t){j->block_id})) {
            j->blocked = 0;
            j->block_kind = BLOCK_NONE;
        }

        if (!j->blocked) {
            if (!best_ready_j ||
                j->effective_priority > best_ready_j->effective_priority) {
                best_ready = job;
                best_ready_j = j;
            }
        } else if (j->block_kind == BLOCK_MUTEX) {
            if (!best_blocked_mutex_j ||
                j->effective_priority >
                    best_blocked_mutex_j->effective_priority) {
                best_blocked_mutex = job;
                best_blocked_mutex_j = j;
            }
        }
    }

    capability_t chosen = best_ready_j ? best_ready : best_blocked_mutex;
    job_data_t *chosen_j = best_ready_j ? best_ready_j : best_blocked_mutex_j;
    if (chosen_j) {
        /* The actual exclusivity guarantee: only one caller's CAS can
         * ever flip this 0 -> 1. If we lose it, someone else's
         * concurrent pick_next_for_node call already claimed this
         * exact job in the tiny window since our scan read it as
         * available -- report nothing found THIS round rather than
         * rescanning (which would risk becoming an unbounded retry
         * loop); the requesting node's own GET_NEXT_JOB caller already
         * treats "not found" as hlt-and-retry, so it tries again on
         * its own, no spinning needed here either. */
        if (!__sync_bool_compare_and_swap(&chosen_j->dispatched, 0, 1)) {
            *out_job = (capability_t){0};
            *out_j = NULL;
            return;
        }
        if (chosen_j->assigned_node == NODE_AFFINITY_ANY) {
            chosen_j->assigned_node = requesting_node_id;
        }
        bind_resource_for(chosen, chosen_j);
    }
    *out_job = chosen;
    *out_j = chosen_j;
}


static void handle_get_next_job(const uint64_t *words, void *response,
                                size_t response_cap, size_t *out_len)
{
    uint64_t requesting_node_id = words[0];
    capability_t job;
    job_data_t *j;
    pick_next_for_node(requesting_node_id, &job, &j);

    uint64_t out[4];
    if (job.handle != 0) {
        out[0] = 1;
        out[1] = job.handle;
        out[2] = (uint64_t)j->type;
        out[3] = j->ctx;
    } else {
        out[0] = 0;
        out[1] = out[2] = out[3] = 0;
    }
    *out_len = capnp_build_flat(response, response_cap, out, 4);
}

static void handle_report_job_result(const uint64_t *words, size_t *out_len)
{
    *out_len = 0;
    capability_t job = {words[0]};
    job_result_t result = (job_result_t)words[1];
    uint64_t block_id = words[2];

    if (result == JOB_FINISHED) {
        /* Read what's needed, then release the lock BEFORE calling
         * destroy_job_object -- it acquires g_storage_lock itself, and
         * this lock isn't reentrant. release_job_resource is a genuine
         * cross-object kernel_invoke to a resource, safe to make
         * unlocked (see pick_next_for_node's own note on why that
         * doesn't touch this lock). */
        storage_lock_acquire_wait();
        job_data_t *j = job_data_for_handle_locked(job);
        uint64_t bound_resource_id = j ? j->bound_resource_id : 0;
        int found = j != NULL;
        trylock_release(&g_storage_lock);
        if (!found) return;
        release_job_resource(job, bound_resource_id);
        destroy_job_object(job);
        return;
    }

    storage_lock_acquire_wait();
    job_data_t *j = job_data_for_handle_locked(job);
    if (j) {
        j->dispatched = 0; /* no longer actively running -- eligible to
                            * be picked again, subject to its own
                            * blocked/eligibility checks below */
        switch (result) {
        case JOB_RUNNING:
            break;
        case JOB_BLOCKED_ON_MUTEX:
            j->blocked = 1;
            j->block_kind = BLOCK_MUTEX;
            j->block_id = block_id;
            break;
        case JOB_BLOCKED_ON_JOB:
            j->blocked = 1;
            j->block_kind = BLOCK_JOB;
            j->block_id = block_id;
            break;
        case JOB_FINISHED:
            break; /* handled above */
        }
    }
    trylock_release(&g_storage_lock);
}

static void handle_mutex_try_lock(const uint64_t *words, void *response,
                                  size_t response_cap, size_t *out_len)
{
    int mutex_id = (int)words[0];
    uint64_t requesting_job_id = words[1];
    uint64_t ok = 0;

    capability_t m = mutex_object_for_id(mutex_id);
    if (m.handle != 0) {
        uint64_t lock_words[1] = {requesting_job_id};
        uint8_t lock_req[24];
        size_t lock_req_len =
            capnp_build_flat(lock_req, sizeof(lock_req), lock_words, 1);
        uint8_t lock_resp[40];
        size_t lock_resp_len = 0;
        if (g_kernel_invoke(m, MUTEX_METHOD_TRY_LOCK, lock_req, lock_req_len,
                            lock_resp, sizeof(lock_resp), &lock_resp_len) &&
            lock_resp_len >= 32) {
            const uint64_t *out = capnp_read_flat(lock_resp);
            ok = out[0];
            uint64_t holder_job_id = out[1];
            if (!ok) {
                capability_t holder = {holder_job_id};
                storage_lock_acquire_wait();
                job_data_t *self_j = job_data_for_handle_locked((capability_t){requesting_job_id});
                job_data_t *holder_j = job_data_for_handle_locked(holder);
                if (self_j && holder_j &&
                    self_j->effective_priority > holder_j->effective_priority) {
                    kprintf("[coreinit]   cluster_scheduler: priority "
                            "inheritance -- boosting job 0x%X from %d to "
                            "%d\n",
                            (unsigned)holder.handle,
                            holder_j->effective_priority,
                            self_j->effective_priority);
                    holder_j->effective_priority = self_j->effective_priority;
                }
                trylock_release(&g_storage_lock);
            }
        }
    }
    *out_len = capnp_build_flat(response, response_cap, &ok, 1);
}

static void handle_mutex_unlock(const uint64_t *words, size_t *out_len)
{
    *out_len = 0;
    int mutex_id = (int)words[0];
    uint64_t requesting_job_id = words[1];

    capability_t m = mutex_object_for_id(mutex_id);
    if (m.handle == 0) {
        return;
    }
    uint64_t unlock_words[1] = {requesting_job_id};
    uint8_t unlock_req[24];
    size_t unlock_req_len =
        capnp_build_flat(unlock_req, sizeof(unlock_req), unlock_words, 1);
    uint8_t unlock_resp[24];
    size_t unlock_resp_len = 0;
    if (g_kernel_invoke(m, MUTEX_METHOD_UNLOCK, unlock_req, unlock_req_len,
                        unlock_resp, sizeof(unlock_resp), &unlock_resp_len) &&
        unlock_resp_len >= 24 && capnp_read_flat(unlock_resp)[0] != 0) {
        storage_lock_acquire_wait();
        job_data_t *self_j = job_data_for_handle_locked((capability_t){requesting_job_id});
        if (self_j) {
            self_j->effective_priority = self_j->base_priority;
        }
        trylock_release(&g_storage_lock);
    }
}

void cluster_scheduler_handler(void *data, uint32_t method_id,
                               const void *msg, size_t len, void *response,
                               size_t response_cap, size_t *out_response_len)
{
    (void)data;
    (void)len;
    *out_response_len = 0;
    const uint64_t *words = capnp_read_flat(msg);

    switch (method_id) {
    case METHOD_JOIN_NODE:
        handle_join_node(words, out_response_len);
        break;
    case METHOD_LEAVE_NODE:
        handle_leave_node(words, out_response_len);
        break;
    case METHOD_REQUEST_ALLOCATION:
        handle_request_allocation(words, response, response_cap, out_response_len);
        break;
    case METHOD_DECLARE_RESOURCE:
        handle_declare_resource(words, response, response_cap, out_response_len);
        break;
    case METHOD_GET_RESOURCE_INFO:
        handle_get_resource_info(words, response, response_cap, out_response_len);
        break;
    case METHOD_ALLOCATE_SLICE:
        handle_allocate_slice(words, response, response_cap, out_response_len);
        break;
    case METHOD_RELEASE_SLICE:
        handle_release_slice(words, response, response_cap, out_response_len);
        break;
    case METHOD_REASSIGN_RESOURCE:
        handle_reassign_resource(words, response, response_cap, out_response_len);
        break;
    case METHOD_SUBMIT_JOB:
        handle_submit_job(words, response, response_cap, out_response_len);
        break;
    case METHOD_GET_NEXT_JOB:
        handle_get_next_job(words, response, response_cap, out_response_len);
        break;
    case METHOD_REPORT_JOB_RESULT:
        handle_report_job_result(words, out_response_len);
        break;
    case METHOD_MUTEX_TRY_LOCK:
        handle_mutex_try_lock(words, response, response_cap, out_response_len);
        break;
    case METHOD_MUTEX_UNLOCK:
        handle_mutex_unlock(words, out_response_len);
        break;
    default:
        break;
    }
}
