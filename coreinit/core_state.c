/* Arch-neutral half of core_state.h: allocation of an AP's core_state
 * through the boot allocator supplied to coreinit. Nothing here needs
 * an architecture backend. core_state_set/core_state_get (the per-core
 * pointer register itself) are the arch-specific half, in
 * core_state_x86_64.c / core_state_aarch64.c. */
#include "core_state.h"
#include "boot_protocol.h"
#include "capnp_build.h"
#include <stddef.h>

struct core_state *core_state_alloc_ap(const struct init_boot_info *info)
{
    if (info && info->ap_core_state_phys) {
        return (struct core_state *)(uintptr_t)info->ap_core_state_phys;
    }
    if (!info || !info->init_description_phys) {
        return NULL;
    }

    const struct init_description_data *description =
        (const struct init_description_data *)(uintptr_t)
        capnp_read_flat((const void *)(uintptr_t)info->init_description_phys);
    if (description->magic != INIT_DESCRIPTION_MAGIC ||
        !description->coreinit_allocator_phys) {
        return NULL;
    }

    struct boot_allocator *allocator = (struct boot_allocator *)(uintptr_t)
        capnp_read_flat((const void *)(uintptr_t)
                        description->coreinit_allocator_phys);
    if (allocator->magic != BOOT_ALLOCATOR_MAGIC || !allocator->alloc) {
        return NULL;
    }

    struct core_state *cs = (struct core_state *)boot_allocator_alloc(
        allocator, sizeof(struct core_state), __alignof__(struct core_state));
    if (!cs) {
        return NULL;
    }

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
