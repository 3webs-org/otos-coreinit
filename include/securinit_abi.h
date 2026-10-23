#pragma once

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
};

#define SECURINIT_BOOT_MARKER 0x5345435552494e49ull
