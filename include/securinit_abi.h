#pragma once

#include <stddef.h>
#include <stdint.h>
#include "cap.h"

/* One-time handoff into independently-linked securinit. Only the standard
 * kernel_invoke entry pointer and ordinary factory/status capabilities
 * cross the image boundary; no C symbol table or direct logging callback
 * is shared. This is still the ring-0 bootstrap phase, not isolation. */
struct securinit_boot_info {
    int (*kernel_invoke)(capability_t target, uint32_t method_id,
                         const void *msg, size_t len, void *response,
                         size_t response_cap, size_t *out_response_len);
    capability_t factory;
    capability_t status_sink;
    /* ARM wrapper-owned EL1/EL0 boundary bootstrap. The factory-created
     * exception-router object accepts a trusted wrapper callback while
     * coreinit's existing VBAR table keeps IRQ/COW routing intact. The
     * wrapper, not saski, owns the user region mapping and gate policy. */
    capability_t exception_router;
    uint32_t exception_router_bind_method;
    uint32_t reserved;
    uint64_t user_region_base;
    uint64_t user_region_size;
    uint64_t ttbr0_root_phys;
    uint64_t x86_pml4_phys;
};

#define SECURINIT_BOOT_MARKER 0x5345435552494e49ull
#define SECURINIT_EL0_DENIED_MARKER 0x454c3044454e4945ull
#define SECURINIT_EL0_IPC_MARKER 0x454c304950434d4bull
#define SECURINIT_RING3_DENIED_MARKER 0x523344454e494544ull
#define SECURINIT_RING3_IPC_MARKER 0x52334950434d4b21ull
