#include "scheduler.h"
#include "capnp_build.h"
#include "cluster_scheduler.h"
#include "serial.h"

/* Method IDs on the cluster scheduler object -- must match
 * cluster_scheduler.c's own METHOD_* defines exactly; duplicated here
 * by necessity, the same reason the kernel/coreinit ABI boundary is
 * duplicated in both headers rather than shared. */
#define CS_METHOD_JOIN_NODE 0
#define CS_METHOD_LEAVE_NODE 1
#define CS_METHOD_REQUEST_ALLOCATION 2
#define CS_METHOD_DECLARE_RESOURCE 3
#define CS_METHOD_REASSIGN_RESOURCE 4
#define CS_METHOD_SUBMIT_JOB 5
#define CS_METHOD_GET_NEXT_JOB 6
#define CS_METHOD_REPORT_JOB_RESULT 7
#define CS_METHOD_MUTEX_TRY_LOCK 8
#define CS_METHOD_MUTEX_UNLOCK 9
#define CS_METHOD_GET_RESOURCE_INFO 10
#define CS_METHOD_ALLOCATE_SLICE 11
#define CS_METHOD_RELEASE_SLICE 12

static job_fn_t g_job_type_table[JOB_TYPE_COUNT];

static int (*g_invoke)(capability_t, uint32_t, const void *, size_t, void *,
                       size_t, size_t *);
static capability_t g_cluster_scheduler;
static uint64_t g_my_node_id;

void scheduler_register_job_type(job_type_t type, job_fn_t fn)
{
    if (type < JOB_TYPE_COUNT) {
        g_job_type_table[type] = fn;
    }
}

void scheduler_client_init(
    int (*invoke_fn)(capability_t, uint32_t, const void *, size_t, void *,
                    size_t, size_t *),
    capability_t cluster_scheduler, uint64_t my_node_id)
{
    g_invoke = invoke_fn;
    g_cluster_scheduler = cluster_scheduler;
    g_my_node_id = my_node_id;
}

static uint64_t submit(uint64_t node_affinity, job_type_t type, uint64_t ctx,
                       int priority, uint64_t resource_id,
                       uint64_t resource_kind, uint64_t resource_min_capacity)
{
    uint64_t words[7] = {node_affinity,       (uint64_t)type,
                         ctx,                 (uint64_t)priority,
                         resource_id,          resource_kind,
                         resource_min_capacity};
    uint8_t req[96];
    size_t req_len = capnp_build_flat(req, sizeof(req), words, 7);
    uint8_t resp[24];
    size_t resp_len = 0;
    if (!g_invoke(g_cluster_scheduler, CS_METHOD_SUBMIT_JOB, req, req_len,
                 resp, sizeof(resp), &resp_len) ||
        resp_len < 24) {
        kprintf("[coreinit]   scheduler: submit_job failed\n");
        return 0;
    }
    return capnp_read_flat(resp)[0];
}

uint64_t scheduler_submit(job_type_t type, void *ctx, int priority,
                          uint64_t resource_id, uint64_t resource_kind,
                          uint64_t resource_min_capacity)
{
    return submit(g_my_node_id, type, (uint64_t)(uintptr_t)ctx, priority,
                 resource_id, resource_kind, resource_min_capacity);
}

uint64_t scheduler_submit_to_node(uint64_t target_node_id, job_type_t type,
                                  uint64_t ctx, int priority,
                                  uint64_t resource_id, uint64_t resource_kind,
                                  uint64_t resource_min_capacity)
{
    return submit(target_node_id, type, ctx, priority, resource_id,
                 resource_kind, resource_min_capacity);
}

uint64_t scheduler_declare_resource(uint64_t kind, uint64_t descriptor,
                                    uint64_t capacity, uint64_t sync_kind)
{
    uint64_t words[5] = {g_my_node_id, kind, descriptor, capacity, sync_kind};
    uint8_t req[64];
    size_t req_len = capnp_build_flat(req, sizeof(req), words, 5);
    uint8_t resp[24];
    size_t resp_len = 0;
    if (!g_invoke(g_cluster_scheduler, CS_METHOD_DECLARE_RESOURCE, req,
                 req_len, resp, sizeof(resp), &resp_len) ||
        resp_len < 24) {
        return 0;
    }
    return capnp_read_flat(resp)[0];
}

int scheduler_reassign_resource(uint64_t resource_id, uint64_t new_owner_node)
{
    uint64_t words[2] = {resource_id, new_owner_node};
    uint8_t req[40];
    size_t req_len = capnp_build_flat(req, sizeof(req), words, 2);
    uint8_t resp[24];
    size_t resp_len = 0;
    if (!g_invoke(g_cluster_scheduler, CS_METHOD_REASSIGN_RESOURCE, req,
                 req_len, resp, sizeof(resp), &resp_len) ||
        resp_len < 24) {
        return 0;
    }
    return capnp_read_flat(resp)[0] != 0;
}

int scheduler_request_allocation(uint64_t base, uint64_t size, uint64_t kind,
                                 uint64_t sync_kind, uint64_t *out_resource_id)
{
    uint64_t words[5] = {g_my_node_id, base, size, kind, sync_kind};
    uint8_t req[64];
    size_t req_len = capnp_build_flat(req, sizeof(req), words, 5);
    uint8_t resp[40];
    size_t resp_len = 0;
    if (!g_invoke(g_cluster_scheduler, CS_METHOD_REQUEST_ALLOCATION, req,
                 req_len, resp, sizeof(resp), &resp_len) ||
        resp_len < 32) {
        if (out_resource_id) *out_resource_id = 0;
        return 0;
    }
    const uint64_t *out = capnp_read_flat(resp);
    if (out_resource_id) *out_resource_id = out[1];
    return out[0] != 0;
}

int scheduler_get_resource_info(uint64_t resource_id, uint64_t *out_owner_node,
                                uint64_t *out_kind, uint64_t *out_descriptor,
                                uint64_t *out_capacity,
                                uint64_t *out_sync_kind)
{
    uint64_t words[1] = {resource_id};
    uint8_t req[24];
    size_t req_len = capnp_build_flat(req, sizeof(req), words, 1);
    uint8_t resp[64];
    size_t resp_len = 0;
    if (!g_invoke(g_cluster_scheduler, CS_METHOD_GET_RESOURCE_INFO, req,
                 req_len, resp, sizeof(resp), &resp_len) ||
        resp_len < 56) {
        if (out_owner_node) *out_owner_node = 0;
        if (out_kind) *out_kind = 0;
        if (out_descriptor) *out_descriptor = 0;
        if (out_capacity) *out_capacity = 0;
        if (out_sync_kind) *out_sync_kind = 0;
        return 0;
    }
    const uint64_t *out = capnp_read_flat(resp);
    int found = out[0] != 0;
    if (out_owner_node) *out_owner_node = out[1];
    if (out_kind) *out_kind = out[2];
    if (out_descriptor) *out_descriptor = out[3];
    if (out_capacity) *out_capacity = out[4];
    if (out_sync_kind) *out_sync_kind = out[5];
    return found;
}

uint64_t scheduler_get_my_node_id(void)
{
    return g_my_node_id;
}

/* Method 11: node_id, size, kind, sync_kind -> ok, resource_id, base.
 * The "mmap, not brk" counterpart to scheduler_request_allocation --
 * caller supplies only a size, gets back both a resource_id (for
 * declaring job dependencies through scheduler_submit, same as any
 * other resource) and the base address mem_mgr's own allocator chose,
 * with no address ever hand-picked or justified by the caller. This is
 * the entry point job_memory.c's slice abstraction is actually built
 * on. */
/* Must only be called while holding ALLOCATOR_MUTEX_ID (see
 * cluster_scheduler.h) -- job_memory_try_acquire_local does this
 * correctly; calling this directly is almost certainly wrong.
 * self_job_id becomes the new resource's reserved_by immediately, so
 * the slice is owned by the acquiring JOB, not merely attributed to
 * its node -- see handle_allocate_slice's own note on why that
 * matters. */
int scheduler_allocate_slice(uint64_t size, uint64_t kind, uint64_t sync_kind,
                             uint64_t self_job_id, uint64_t *out_resource_id,
                             uint64_t *out_base)
{
    uint64_t words[5] = {g_my_node_id, self_job_id, size, kind, sync_kind};
    uint8_t req[64];
    size_t req_len = capnp_build_flat(req, sizeof(req), words, 5);
    uint8_t resp[48];
    size_t resp_len = 0;
    if (!g_invoke(g_cluster_scheduler, CS_METHOD_ALLOCATE_SLICE, req, req_len,
                 resp, sizeof(resp), &resp_len) ||
        resp_len < 32) {
        if (out_resource_id) *out_resource_id = 0;
        if (out_base) *out_base = 0;
        return 0;
    }
    const uint64_t *out = capnp_read_flat(resp);
    if (out_resource_id) *out_resource_id = out[1];
    if (out_base) *out_base = out[2];
    return out[0] != 0;
}

/* Must only be called while holding ALLOCATOR_MUTEX_ID. self_job_id
 * must match the resource's own reserved_by (the job that acquired it)
 * or the release is refused -- see job_memory_release, the intended
 * caller. */
int scheduler_release_slice(uint64_t resource_id, uint64_t self_job_id)
{
    uint64_t words[2] = {resource_id, self_job_id};
    uint8_t req[40];
    size_t req_len = capnp_build_flat(req, sizeof(req), words, 2);
    uint8_t resp[24];
    size_t resp_len = 0;
    if (!g_invoke(g_cluster_scheduler, CS_METHOD_RELEASE_SLICE, req, req_len,
                 resp, sizeof(resp), &resp_len) ||
        resp_len < 24) {
        return 0;
    }
    return capnp_read_flat(resp)[0] != 0;
}

int scheduler_mutex_try_lock(int mutex_id, uint64_t self_job_id)
{
    uint64_t words[2] = {(uint64_t)mutex_id, self_job_id};
    uint8_t req[40];
    size_t req_len = capnp_build_flat(req, sizeof(req), words, 2);
    uint8_t resp[24];
    size_t resp_len = 0;
    if (!g_invoke(g_cluster_scheduler, CS_METHOD_MUTEX_TRY_LOCK, req, req_len,
                 resp, sizeof(resp), &resp_len) ||
        resp_len < 24) {
        return 0;
    }
    return capnp_read_flat(resp)[0] != 0;
}

void scheduler_mutex_unlock(int mutex_id, uint64_t self_job_id)
{
    uint64_t words[2] = {(uint64_t)mutex_id, self_job_id};
    uint8_t req[40];
    size_t req_len = capnp_build_flat(req, sizeof(req), words, 2);
    g_invoke(g_cluster_scheduler, CS_METHOD_MUTEX_UNLOCK, req, req_len, NULL,
            0, NULL);
}

void scheduler_run(void)
{
    kprintf("[coreinit] scheduler: handing off execution control to the "
            "cluster scheduler -- this node is now a pull-based worker, "
            "no local job queue left to fall back on\n");

    uint64_t self_req_words[1] = {g_my_node_id};
    uint8_t self_req[24];
    size_t self_req_len = capnp_build_flat(self_req, sizeof(self_req),
                                           self_req_words, 1);

    for (;;) {
        uint8_t resp[64];
        size_t resp_len = 0;
        if (!g_invoke(g_cluster_scheduler, CS_METHOD_GET_NEXT_JOB, self_req,
                     self_req_len, resp, sizeof(resp), &resp_len) ||
            resp_len < 48) {
            __asm__ volatile("hlt");
            continue;
        }

        const uint64_t *out = capnp_read_flat(resp);
        uint64_t found = out[0];
        if (!found) {
            __asm__ volatile("hlt");
            continue;
        }

        uint64_t job_id = out[1];
        uint64_t job_type = out[2];
        uint64_t ctx = out[3];

        job_result_t result = JOB_FINISHED;
        uint64_t block_id = 0;
        if (job_type < JOB_TYPE_COUNT && g_job_type_table[job_type]) {
            result = g_job_type_table[job_type]((void *)(uintptr_t)ctx,
                                                job_id, &block_id);
        } else {
            kprintf("[coreinit]   scheduler: got unknown job type %u, "
                    "reporting finished\n",
                    (unsigned)job_type);
        }

        uint64_t report_words[3] = {job_id, (uint64_t)result, block_id};
        uint8_t report_req[48];
        size_t report_req_len =
            capnp_build_flat(report_req, sizeof(report_req), report_words, 3);
        g_invoke(g_cluster_scheduler, CS_METHOD_REPORT_JOB_RESULT, report_req,
                report_req_len, NULL, 0, NULL);
    }
}
