#pragma once

#include <stdint.h>

/* The new boot handoff: a bootloader (otos-grub or otos-boot, never
 * saski/GRUB's xnu loader) does Mach-O loading but deliberately does
 * not parse the firmware's FDT/ACPI description. It establishes the
 * stack and active address space, then hands saski an already-resolved
 * entry point plus one pointer. saski forwards the raw FDT/RSDP and
 * opaque InitDescription into init_boot_info, then jumps to the entry
 * point. The caller retains responsibility for
 * the execution context it used to enter saski; saski neither allocates
 * a replacement stack nor constructs replacement page tables.
 *
 * Mach-O entry point convention: the standard `LC_MAIN` load command
 * (entryoff into __TEXT), exactly what ld64.lld already emits for an
 * ordinary PIE Mach-O with no special handling. There is no new
 * convention to invent here -- the ONLY reason this project's existing
 * build ever rewrote this to `LC_UNIXTHREAD` (see saski's
 * tools/macho_to_xnu.py) was to satisfy GRUB's xnu loader, which this
 * whole protocol replaces. A loader implementing this protocol reads
 * `LC_MAIN` the way any ordinary Mach-O loader would.
 *
 * Register mapping at saski's entry, three values:
 *
 *   aarch64 (standard arm64 boot-protocol registers, extended):
 *     x0 = a real, standard FDT physical address. Kept so the
 *          resulting image still looks like a normal arm64 boot target
 *          to anything that only speaks the standard protocol. Forwarded
 *          as raw input for coreinit's own FDT parsing.
 *     x1 = init's already-resolved entry point.
 *     x2 = InitDescription object's physical address. Opaque to
 *          saski; forwarded into init_boot_info for coreinit's own SMP
 *          bring-up to consult if a secondary core turns out to be a
 *          different architecture than the BSP.
 *   x86_64 (SysV integer-argument registers):
 *     rdi = raw ACPI RSDP physical address for coreinit to parse.
 *     rsi = init's already-resolved entry point.
 *     rdx = InitDescription object's physical address.
 *
 * Repo hosting: otos-coreinit owns this header, the same way it
 * already owns every other wire-format/ABI header in this project
 * (init_abi.h, smp_layout.h). Vendored byte-identically into saski
 * (same convention those two already use), and vendored again into
 * otos-grub/otos-boot as their own copies -- genuinely separate repos,
 * no shared git history to lean on. */

/* ---- The boot allocator object: a flat, pointer-free Cap'n Proto
 * struct -- seven UInt64 data words, no pointer fields, built/read the
 * same way coreinit/capnp_build.h's capnp_build_flat/capnp_read_flat
 * already handle any other flat data-only struct in this project.
 * Carries coreinit's real, callable bump allocator over a fixed region,
 * rather than merely handing coreinit a base+length pair and requiring
 * it to supply its own allocator implementation. Saski treats this
 * object as opaque and only forwards its address. In particular, saski
 * does not allocate stacks or page tables from this region: the entity
 * that enters saski owns that execution context, and coreinit owns the
 * context it constructs when it starts an AP. coreinit's allocator is
 * open-ended -- core_state_alloc_ap may keep allocating from it as APs
 * come online -- so saski must never finalize it.
 *
 * alloc/finalize are stored as plain UInt64 code addresses, not native
 * C function-pointer fields, and every caller casts-and-calls at the
 * use site (see e.g. coreinit/core_state.c's core_state_alloc_ap) --
 * deliberately, not merely to keep this struct capnp-flat. A native
 * function-pointer field's own SIZE varies by reader (4 bytes on
 * 32-bit x86, 8 on 64-bit), and this struct can genuinely be read by
 * more than one width: the same physical bytes may be read once by a
 * 32-bit-mode x86 core early in boot and again by the same core after
 * it enters long mode, or (per InitDescription's own heterogeneous-
 * core support) by an entirely different architecture's core. A
 * native-width field would put region_base/region_len/used/finalized
 * at different byte offsets depending on who's reading -- a fixed
 * UInt64 cannot. (The alloc/finalize CODE itself is still only ever
 * callable by a core of the same architecture it was compiled for --
 * no encoding fixes that; this only fixes the STRUCT's own layout
 * being reader-width-independent.)
 *
 * The allocator's own machine code must be copied into the SAME
 * reserved region this struct's own region_base/region_len describe
 * (or another region that outlives it) -- never left resident only in
 * the bootloader's own, otherwise-reclaimable memory. alloc/finalize
 * are called long after the bootloader that built this object has
 * handed off control and may have nothing left reserved for its own
 * use. */
#define BOOT_ALLOCATOR_MAGIC 0x424f4f54414c4f43ull /* "BOOTALOC" */
#define BOOT_ALLOCATOR_WORDS 7 /* sizeof(struct boot_allocator) / 8 --
                               * keep in sync by hand, same convention
                               * as every other wire-shape constant in
                               * this project. */
struct boot_allocator {
    uint64_t magic; /* BOOT_ALLOCATOR_MAGIC -- sanity check, not a
                     * negotiated version yet */
    /* void *(*)(struct boot_allocator *self, uint64_t size,
     * uint64_t align), stored as a plain code address -- see this
     * struct's own top comment for why not a native function-pointer
     * field. Bump-allocates size bytes, aligned to `align` (treated as
     * 16 if 0). Returns the allocated region's physical address, or
     * NULL if it would not fit in [region_base, region_base +
     * region_len), or if finalize (below) has already run. */
    uint64_t alloc;
    /* void (*)(struct boot_allocator *self), same storage convention
     * as alloc above. Shrinks region_len down to used (the real
     * high-water mark) and makes every subsequent alloc call fail,
     * unconditionally -- explicitly, via the `finalized` field below,
     * not merely as a side effect of region_len becoming equal to used
     * (a zero-size alloc could otherwise still "succeed" right at that
     * boundary). Meant to be called once a consumer is done allocating
     * from its own pool for good, so whatever's left at the tail of
     * region_base+region_len is known, precisely, to be free -- this
     * struct does not itself return that tail to any other allocator;
     * a caller that wants to is free to claim
     * [region_base + region_len, original region_len) itself after
     * calling this. */
    uint64_t finalize;
    uint64_t region_base;
    uint64_t region_len; /* shrunk by finalize() to the real high-water
                         * mark; starts as the full reserved extent */
    uint64_t used;
    uint64_t finalized; /* 0 until finalize() runs */
};

/* Cast-and-call wrappers around alloc/finalize's raw UInt64 code
 * addresses -- every caller uses these, not a hand-rolled cast, so
 * there is exactly one place that knows the real function-pointer
 * type on each side of the call. */
typedef void *(*boot_allocator_alloc_fn)(struct boot_allocator *self,
                                         uint64_t size, uint64_t align);
typedef void (*boot_allocator_finalize_fn)(struct boot_allocator *self);

static inline void *boot_allocator_alloc(struct boot_allocator *self,
                                         uint64_t size, uint64_t align)
{
    boot_allocator_alloc_fn fn =
        (boot_allocator_alloc_fn)(uintptr_t)self->alloc;
    return fn(self, size, align);
}

static inline void boot_allocator_finalize(struct boot_allocator *self)
{
    boot_allocator_finalize_fn fn =
        (boot_allocator_finalize_fn)(uintptr_t)self->finalize;
    fn(self);
}

#define BOOT_PROTO_ARCH_X86_64 0
#define BOOT_PROTO_ARCH_AARCH64 1

/* ---- InitDescription: real Cap'n Proto object, opaque to saski ----
 *
 * A list of per-architecture init-binary variants, for coreinit's own
 * SMP bring-up to consult if a secondary core turns out to be a
 * different architecture than the BSP -- the original, still-valid
 * reason this object exists (see otos-grub/otos-boot's own READMEs).
 * Two data words (a magic and the coreinit boot allocator's physical
 * address) and one pointer field, a composite list of InitVariant. */
#define BOOT_PROTO_ID_DATA_WORDS 2
#define INIT_DESCRIPTION_MAGIC 0x494e495444455343ull /* "INITDESC" */

struct init_description_data {
    uint64_t magic;
    uint64_t coreinit_allocator_phys;
};

/* One entry in InitDescription's variant list: which architecture this
 * variant is for, and where its kernel (saski) and init (coreinit)
 * Mach-O images live -- physical address + length each, the same shape
 * init_boot_info's own now-superseded kernel_image_addr/len and
 * init_image_addr/len pair already used for a single architecture. */
struct init_variant {
    uint64_t arch_tag; /* BOOT_PROTO_ARCH_X86_64/AARCH64 */
    uint64_t kernel_macho_phys;
    uint64_t kernel_macho_len;
    uint64_t init_macho_phys;
    uint64_t init_macho_len;
};
#define BOOT_PROTO_INIT_VARIANT_WORDS 5
