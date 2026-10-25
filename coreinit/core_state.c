/* Arch-neutral half of core_state.h: the AP bump allocator. Pure
 * bookkeeping over a flat range of bytes handed down at boot -- nothing
 * here needs an architecture backend. core_state_set/core_state_get (the
 * per-core pointer register itself) are the arch-specific half, in
 * core_state_x86_64.c / core_state_aarch64.c. */
#include "core_state.h"
#include <stddef.h>

/* Seeded from info on the first call, then just bumped. This core's own
 * bring-up loop calls this sequentially (never concurrently -- the BSP
 * brings up one AP at a time), so a plain pair of globals is sufficient;
 * no lock. */
static uint64_t g_pool_next = 0;
static uint64_t g_pool_end = 0;
static int g_pool_seeded = 0;

struct core_state *core_state_alloc_ap(const struct init_boot_info *info)
{
    if (!g_pool_seeded) {
        g_pool_seeded = 1;
        g_pool_next = info->free_memory_base;
        g_pool_end = info->free_memory_base + info->free_memory_len;
    }

    if (g_pool_next == 0 ||
        sizeof(struct core_state) > g_pool_end - g_pool_next) {
        return NULL;
    }

    struct core_state *cs = (struct core_state *)(uintptr_t)g_pool_next;
    g_pool_next += sizeof(struct core_state);

    uint8_t *bytes = (uint8_t *)cs;
    for (size_t i = 0; i < sizeof(struct core_state); i++) {
        bytes[i] = 0;
    }
    return cs;
}

void core_state_init_defaults(struct core_state *cs)
{
    cs->core_tag = -1;
    cs->current_job = -1;
}
