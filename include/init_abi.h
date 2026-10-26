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
     * THIS core's own capability table.
     *
     * THIS FIELD IS ALSO THE CANONICAL BSP/AP ROLE SIGNAL: 0 means this
     * boot_info is the BSP's own, nonzero means it's an AP's. Nothing in
     * the kernel needs to tell the two apart (kernel_boot and
     * kernel_boot_ap are already separate functions), but coreinit_main
     * is one shared entry point for both, with no other way to know
     * which one it was handed -- see coreinit/core_state.c's own use of
     * exactly this check. Deliberately not a separate dedicated field:
     * this value is already unambiguous (every BSP boot_info genuinely
     * has no BSP of its own to reach, every AP's genuinely does), so a
     * second field would just be two ways to say the same thing that
     * could drift out of sync with each other. */
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
    uint64_t fdt_blob_phys; /* Flattened Device Tree physical address, as
                             * delivered by the platform's own boot
                             * convention. On aarch64 the standard arm64 boot
                             * protocol passes it in x0 at kernel entry, and
                             * the kernel's aarch64 boot path captures it
                             * verbatim -- see include/fdt.h's own note on why
                             * it must then be validated before it is trusted.
                             * On x86_64 this stays 0: that platform's device
                             * description is ACPI, which reaches init through
                             * early_rsdp_phys above instead. The two fields
                             * are deliberately separate rather than one
                             * overloaded "arch hint": they carry different
                             * formats read by different code, and a single
                             * field would invite treating one as the other.
                             * 0 means "not supplied", which is a normal value
                             * on a platform with no device tree, not an error. */
    uint64_t ram_size; /* Total RAM, from GRUB's own real memory-map
                        * discovery -- see kernel/xnu_boot_args.h and
                        * boot.S's own _g_xnu_boot_args_addr for where
                        * this actually comes from: XNU's real boot
                        * protocol, not a value this kernel invented.
                        * 0 if boot_args wasn't in the expected v2 shape
                        * (checked explicitly by kernel_boot, never
                        * assumed) -- callers must treat 0 as "unknown",
                        * not "no RAM". */
    uint64_t free_memory_base; /* Start of a pool of genuinely free
                                * memory init may carve its own
                                * allocations from (currently: one
                                * struct core_state per AP -- see
                                * coreinit/core_state.h). 0/0 means "none
                                * supplied"; init must treat that as
                                * "nothing to allocate from", not assume
                                * some default region. Distinct from
                                * mem_mgr's own claim-based allocator:
                                * this is raw, unclaimed memory handed
                                * down at boot, before mem_mgr exists to
                                * claim anything. */
    uint64_t free_memory_len;

    /* The new boot-protocol handoff (see include/boot_protocol.h) --
     * populated only by a loader implementing that protocol
     * (otos-grub, otos-boot), zero/absent on every boot path that
     * still exists in this repo today (GRUB's xnu loader, mkstub.py).
     * machine_description_phys/init_description_phys are forwarded
     * here unread -- saski copies the register values
     * (aarch64 x1/x2, x86_64 rdi/rsi) straight into these two fields
     * and never looks inside either object; only coreinit decodes
     * them (coreinit/boot_protocol_decode.h). Once a real loader
     * exists, free_memory_base/free_memory_len above should be sourced
     * from MachineDescription's own freeMemoryBase/freeMemoryLen
     * fields instead of wherever they come from today -- not changed
     * in this pass, which only adds the new fields, not the wiring. */
    uint64_t machine_description_phys;
    uint64_t init_description_phys;
    /* saski_pool_base/saski_pool_len: memory the loader set aside FOR
     * SASKI'S OWN use (see struct saski_boot_params's own comment in
     * boot_protocol.h for exactly what saski allocates from it --
     * per-AP stacks, and on x86_64 per-AP page tables). Reported here
     * purely so init's own memory manager marks this range reserved;
     * init must never allocate from it -- that is what
     * free_memory_base/free_memory_len above are for. 0/0 means "none
     * supplied", the same convention every other optional field in
     * this struct already uses. */
    uint64_t saski_pool_base;
    uint64_t saski_pool_len;
};
