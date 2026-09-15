#pragma once

#include "cap.h"

/* The one convention the kernel and userspace init must agree on ahead
 * of time: the shape of the boot handoff, and that init's entry point
 * is an ordinary cdecl C function of this signature. This is the
 * kernel's one irreducible, hardcoded bootstrap assumption -- there is
 * no earlier userspace authority available to have negotiated anything
 * richer with.
 *
 * Deliberately generic: this project's own reference init
 * implementation lives in coreinit/ and is a genuinely complete,
 * capable one, but the kernel itself has no idea that's what it's
 * loading, or that "coreinit" exists as a name at all -- nothing in
 * kernel/ ever references it. Any binary matching this same handoff
 * shape (a Mach-O image the kernel embeds and loads, exposing one
 * entry point of this signature) boots exactly the same way. This
 * mirrors how the kernel already treats an image's own load address --
 * see kernel/macho.h's own note on why it honors init's self-declared
 * vmaddr rather than deciding one -- extended to identity as well:
 * the kernel has no opinion about WHAT it's booting, only about the
 * shape of the handoff itself.
 *
 * Nothing else about init's binary format, layout, or internal
 * implementation is assumed anywhere else in the kernel: the kernel
 * embeds it directly (see kernel/boot.S's .incbin), Mach-O-loads it,
 * and calls this one symbol.
 *
 * Because init is a genuinely separate, independently-linked binary
 * (not statically linked into the kernel), it cannot call kernel_invoke
 * as an ordinary compiled-in function call -- there is no shared symbol
 * table between two independently loaded binaries. The kernel hands over
 * a function pointer to its one syscall instead. This is the
 * ring-0-monolithic-phase stand-in for what becomes a real trap/syscall
 * boundary once init (its own choice, not the kernel's) introduces
 * actual privilege separation: the call site in init does not need
 * to change when that happens, only what sits behind this one pointer
 * does.
 *
 * The factory (object-creation) object's own handle travels explicitly,
 * in caps.factory below -- not as a well-known constant both binaries
 * agree on by including the same header. This is a deliberate choice,
 * not an oversight: this kernel doesn't get to assume init knows
 * anything about it ahead of time, including its identity, for the
 * same reason it doesn't assume init's own name or binary format (see
 * this struct's own name, and macho.h's note on target_base). Once
 * init has this handle, everything else (creating objects, destroying
 * them, wrapping) happens by invoking it or whatever it creates,
 * through this same syscall.
 *
 * The struct's field type must exactly match kernel/cap.h's real
 * kernel_invoke signature -- this is duplicated by necessity (kernel and
 * init are compiled and linked completely independently), not by
 * accident. */
struct init_boot_info {
    initial_capabilities_t caps; /* .factory is the one thing init
                                  * cannot discover any other way -- see
                                  * this struct's own top comment. */
    /* The three fields below exist purely to make AP bring-up possible:
     * they let init (running on the BSP) construct a fresh, correct
     * copy of the kernel for another core without needing to parse the
     * kernel's own ELF symbol table to find kernel_boot_ap's address --
     * the BSP's own kernel_boot already knows it (same binary, ordinary
     * C reference), so it's simplest to just hand it over the same way
     * kernel_invoke itself is. */
    int (*kernel_invoke)(capability_t target, uint32_t method_id,
                         const void *msg, size_t len, void *response,
                         size_t response_cap, size_t *out_response_len);
    uint64_t kernel_load_base;   /* KERNEL_LOAD_BASE -- where THIS running
                                  * copy of the kernel is linked to load,
                                  * needed to compute the delta when
                                  * copying it elsewhere for another core */
    uint64_t kernel_boot_ap_addr; /* kernel_boot_ap's address in THIS copy */
    uint64_t pml4_phys;           /* boot.S's identity-map root, reused
                                   * as-is for every core (see kernel.c's
                                   * own note on why this is sound for now) */
    /* init_image_addr/len: this kernel's own embedded copy of init's
     * Mach-O file bytes (see boot.S's .incbin), still unparsed --
     * distinct from wherever macho_load already placed the BSP's own
     * running copy of init. Needed so init can run a FRESH macho_load
     * against these same original bytes for each additional core,
     * rather than needing to copy an already-loaded (and already
     * entangled with the BSP's own addresses) image.
     *
     * kernel_image_addr/len: this kernel's own currently-loaded image
     * (kernel_load_base is where it starts; macho_image_size is how
     * this kernel itself measures how far it extends -- see that
     * function's own comment for why a raw span, not a re-load). Used
     * the opposite way from init's own: copied byte-for-byte, not
     * re-parsed, since this kernel is PIE and already correctly laid
     * out in memory, and only PIE/RIP-relative code (kernel_boot_ap
     * and whatever it calls) ever actually runs from within the copy.
     *
     * Either pair is zero/absent if the corresponding image wasn't
     * available -- SMP bring-up simply isn't possible without both,
     * checked explicitly rather than assumed present. */
    uint64_t init_image_addr;
    uint64_t init_image_len;
    uint64_t kernel_image_addr;
    uint64_t kernel_image_len;
    /* Discovered shared services -- zero/absent for the BSP's own
     * boot_info (it hosts these locally and already has their handles
     * directly from creating them), populated for an AP's boot_info by
     * kernel_boot_ap reading the discovery record its own region's
     * CORE_DISCOVERY_OFFSET holds (see smp_layout.h). Reaching a
     * BSP-hosted object means calling bsp_kernel_invoke_addr directly
     * with the given handle -- a different function pointer than this
     * same struct's own kernel_invoke field, which only ever reaches
     * THIS core's own capability table. */
    uint64_t bsp_kernel_invoke_addr;
    uint64_t mutex_manager_handle;
    uint64_t memory_manager_handle;
    uint64_t hardware_mutex_id;
    uint64_t lapic_calibration_count; /* counts-per-10ms, measured once
                                       * by the BSP; see lapic_timer.h */
    uint64_t cluster_scheduler_handle; /* discovered handle, meaningful
                                        * only via bsp_kernel_invoke_addr;
                                        * 0/unused on the BSP's own
                                        * boot_info, which creates this
                                        * object locally instead -- see
                                        * cluster_scheduler.h */
    uint64_t serial_resource_handle; /* the one shared "serial" resource
                                      * object -- reserved briefly by
                                      * job_kprintf around each print, to
                                      * keep two cores' output from
                                      * interleaving mid-line. Discovered
                                      * the same way as
                                      * cluster_scheduler_handle for an
                                      * AP; the BSP sets this directly
                                      * after creating the resource
                                      * itself. */
    uint64_t own_mailbox_base; /* where THIS core's own scheduler should
                               * poll for cross-core job delivery --
                               * BSP_MAILBOX_REGION_BASE + offset for the
                               * BSP, region_base + offset for an AP; see
                               * smp_layout.h's CORE_MAILBOX_OFFSET */
    uint64_t early_rsdp_phys; /* ACPI RSDP physical address, found by
                              * kernel.c's own xnu_boot_args_find_rsdp
                              * (real EFI configuration-table lookup)
                              * -- see that function's own comment for
                              * why init's own acpi_discover can't just
                              * re-scan for it itself on this boot
                              * path. 0 if not found (acpi_discover
                              * falls back to its own legacy-BIOS scan
                              * in that case -- see that function's own
                              * comment). */
    uint64_t ram_size; /* Total RAM, from GRUB's own real memory-map
                        * discovery -- see kernel/xnu_boot_args.h and
                        * boot.S's own _g_xnu_boot_args_addr for where
                        * this actually comes from: XNU's real boot
                        * protocol, not a value this kernel invented.
                        * 0 if boot_args wasn't in the expected v2 shape
                        * (checked explicitly by kernel_boot, never
                        * assumed) -- callers must treat 0 as "unknown",
                        * not "no RAM". */
};
