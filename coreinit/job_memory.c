#include "job_memory.h"
#include "arch_mmu.h"
#include "cluster_scheduler.h"
#include "pagefault.h"
#include "serial.h"

static uint64_t g_my_pml4_phys;

void job_memory_init(uint64_t my_pml4_phys)
{
    g_my_pml4_phys = my_pml4_phys;
}

job_result_t job_memory_try_acquire_local(uint64_t size, uint64_t kind,
                                          uint64_t self_job_id,
                                          uint64_t *out_block_id,
                                          job_memory_slice_t *out)
{
    if (!scheduler_mutex_try_lock(ALLOCATOR_MUTEX_ID, self_job_id)) {
        *out_block_id = ALLOCATOR_MUTEX_ID;
        return JOB_BLOCKED_ON_MUTEX;
    }

    uint64_t resource_id = 0, base = 0;
    scheduler_allocate_slice(size, kind, RESOURCE_SYNC_TRANSFER, self_job_id,
                             &resource_id, &base);
    scheduler_mutex_unlock(ALLOCATOR_MUTEX_ID, self_job_id);

    out->resource_id = resource_id;
    out->base = base;
    out->size = size;
    out->sync_kind = RESOURCE_SYNC_TRANSFER;
    out->owned = (resource_id != 0);
    return JOB_FINISHED;
}

job_result_t job_memory_try_release(uint64_t resource_id, uint64_t self_job_id,
                                    uint64_t *out_block_id, int *out_ok)
{
    if (!scheduler_mutex_try_lock(ALLOCATOR_MUTEX_ID, self_job_id)) {
        *out_block_id = ALLOCATOR_MUTEX_ID;
        return JOB_BLOCKED_ON_MUTEX;
    }
    int ok = scheduler_release_slice(resource_id, self_job_id);
    scheduler_mutex_unlock(ALLOCATOR_MUTEX_ID, self_job_id);
    if (out_ok) {
        *out_ok = ok;
    }
    return JOB_FINISHED;
}

int job_memory_acquire_resource(uint64_t resource_id, job_memory_slice_t *out)
{
    uint64_t owner, kind, descriptor, capacity, sync_kind;
    if (!scheduler_get_resource_info(resource_id, &owner, &kind, &descriptor,
                                     &capacity, &sync_kind)) {
        return 0;
    }

    out->resource_id = resource_id;
    out->base = descriptor;
    out->size = capacity;
    out->sync_kind = sync_kind;
    out->owned = (owner == scheduler_get_my_node_id());

    if (out->owned || sync_kind == RESOURCE_SYNC_TRANSFER) {
        /* Already ours, or a TRANSFER resource we don't own -- no
         * fault-driven path exists for TRANSFER, so this only
         * succeeds if we ARE the owner. */
        return out->owned;
    }

    /* STREAM or COW, owned elsewhere: local fault handling so touching
     * [base, base+size) transparently faults, resolves, and behaves
     * like ordinary memory from then on -- see pagefault.h. This
     * entire mechanism is pagefault.c splitting a region and catching
     * faults on THIS core -- a node with no MMU has nothing to split
     * or catch faults with, so it can't be a non-owning accessor of a
     * STREAM/COW resource no matter what the owner declared. The
     * owner side of this same requirement is enforced at declaration
     * time (cluster_scheduler.c's handle_declare_resource); this is
     * the other half, checked here rather than assumed, since a
     * different-architecture node could reach this code with no MMU
     * even though every node on THIS architecture always has one (see
     * arch_mmu.h). */
    if (!arch_has_mmu()) {
        return 0;
    }

    pagefault_init(g_my_pml4_phys);

    uint64_t region_start = descriptor & ~0x1FFFFFull;
    uint64_t region_end = (descriptor + capacity + 0x1FFFFFull) & ~0x1FFFFFull;
    for (uint64_t region = region_start; region < region_end;
         region += 0x200000ull) {
        pagefault_split_region(g_my_pml4_phys, region);
    }

    uint64_t page_start = descriptor & ~0xFFFull;
    uint64_t page_end = (descriptor + capacity + 0xFFFull) & ~0xFFFull;
    for (uint64_t page = page_start; page < page_end; page += 0x1000ull) {
        if (sync_kind == RESOURCE_SYNC_STREAM) {
            pagefault_mark_absent(g_my_pml4_phys, page);
        } else {
            pagefault_mark_cow(g_my_pml4_phys, page);
        }
    }

    return 1;
}
