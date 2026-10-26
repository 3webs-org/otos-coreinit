#pragma once

#include <stdint.h>

/* The new boot handoff: a bootloader (otos-grub or otos-boot, never
 * saski/GRUB's xnu loader) does all Mach-O and machine-description
 * parsing itself, and hands saski an already-resolved entry point plus
 * three pointers. saski's own job is to forward two of them into
 * init_boot_info unread, read the third directly, and jump to the
 * entry point -- see each struct's own comment below for exactly which
 * is which and why.
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
 * Register mapping at saski's entry, five values (the fourth slot used
 * to be "the entry point"; SaskiBootParams inserts before it):
 *
 *   aarch64 (standard arm64 boot-protocol registers, extended):
 *     x0 = a real, standard FDT physical address. Kept so the
 *          resulting image still looks like a normal arm64 boot target
 *          to anything that only speaks the standard protocol -- saski
 *          itself never reads it.
 *     x1 = MachineDescription object's physical address. Opaque to
 *          saski; forwarded into init_boot_info for coreinit to decode.
 *     x2 = InitDescription object's physical address. Opaque to
 *          saski; forwarded into init_boot_info for coreinit's own SMP
 *          bring-up to consult if a secondary core turns out to be a
 *          different architecture than the BSP.
 *     x3 = SaskiBootParams struct's physical address. saski reads this
 *          one directly (see that struct's own comment).
 *     x4 = init's already-resolved entry point.
 *
 *   x86_64 (SysV integer-argument registers, by analogy -- no
 *   FDT-equivalent slot: unlike aarch64, x86 has no single universal
 *   "looks like a normal boot target" register convention worth
 *   preserving here):
 *     rdi = MachineDescription object's physical address.
 *     rsi = InitDescription object's physical address.
 *     rdx = SaskiBootParams struct's physical address.
 *     rcx = init's already-resolved entry point.
 *
 * Repo hosting: otos-coreinit owns this header, the same way it
 * already owns every other wire-format/ABI header in this project
 * (init_abi.h, smp_layout.h). Vendored byte-identically into saski
 * (same convention those two already use), and vendored again into
 * otos-grub/otos-boot as their own copies -- genuinely separate repos,
 * no shared git history to lean on. */

/* ---- SaskiBootParams: plain C struct, saski reads it directly ----
 *
 * NOT Cap'n Proto-encoded, deliberately: unlike the two objects below,
 * there is no cross-binary-opacity reason to encode this one -- saski
 * is the only reader, the same reasoning init_boot_info itself already
 * uses for every field it defines.
 *
 * Carries a pool of memory the bootloader sets aside FOR SASKI'S OWN
 * USE -- not a pool init is invited to allocate from. Once saski's old
 * per-core 4 MiB region scheme (smp_layout.h's CORE_REGION_BASE/
 * CORE_STACK_OFFSET/CORE_PGTABLE_OFFSET) is retired, saski still needs
 * somewhere to bump-allocate each AP's own stack from before that core
 * can be handed off to coreinit at all, and (x86_64 only)
 * each AP's own page table (kernel.c's build_ap_page_tables -- a
 * genuinely distinct table per AP, unlike aarch64's shareable single
 * identity map). Both are saski's own bookkeeping. Reporting this
 * pool's extent into init_boot_info (saski_pool_base/saski_pool_len)
 * is purely so init's own memory manager marks the range reserved and
 * never touches it -- NOT an invitation for init to allocate from it.
 * The separate pool init genuinely does allocate from (an AP's own
 * struct core_state) is MachineDescription's freeMemoryBase/
 * freeMemoryLen below -- a different pool, read directly by coreinit;
 * saski never needs to see that one. */
struct saski_boot_params {
    uint64_t magic; /* SASKI_BOOT_PARAMS_MAGIC -- sanity check, not a
                     * negotiated version yet */
    uint64_t saski_pool_base;
    uint64_t saski_pool_len;
};
#define SASKI_BOOT_PARAMS_MAGIC 0x53414b4942504d31ull /* "SAKIBPM1" */

/* ---- MachineDescription: real Cap'n Proto object, opaque to saski ----
 *
 * A wire-encoding of a superset of coreinit/topology.h's own existing
 * topology_t -- the loader does the ACPI/FDT parsing topology_discover()
 * does today, once, and hands coreinit the result pre-digested instead
 * of a raw table to walk itself. Built/read with capnp_list.h's
 * capnp_build_list_root/capnp_read_list_root: BOOT_PROTO_MD_DATA_WORDS
 * plain data words (this struct, in exactly this field order) followed
 * by one pointer field, a composite list of CoreEntry.
 *
 * Every field below is present on both architectures; zero means "not
 * supplied / not applicable on this architecture", the same
 * deliberately-separate-fields, zero-means-absent convention
 * init_abi.h already uses for early_rsdp_phys/fdt_blob_phys. fdt_blob_phys
 * and early_rsdp_phys themselves both survive here as raw escape
 * hatches (smp_aarch64.c still reads the FDT directly for PSCI
 * cpu-node lookups) even though the loader has already parsed them
 * once -- not a contradiction, just "give coreinit the pre-digested
 * answer AND the raw source it came from". */
struct machine_description_data {
    uint64_t magic; /* MACHINE_DESCRIPTION_MAGIC */
    uint64_t arch_tag; /* BOOT_PROTO_ARCH_X86_64/AARCH64 */
    uint64_t ram_size;
    uint64_t free_memory_base; /* the pool init itself allocates from,
                                * e.g. an AP's own struct core_state --
                                * see core_state.h's core_state_alloc_ap.
                                * NOT saski's own pool (see
                                * saski_boot_params above). */
    uint64_t free_memory_len;
    uint64_t fdt_blob_phys;    /* aarch64; 0 on x86_64 */
    uint64_t early_rsdp_phys;  /* x86_64; 0 on aarch64 */
    uint64_t uart_base;        /* aarch64 PL011; 0 on x86_64 (fixed port,
                                * nothing to discover) */
    uint64_t lapic_base;       /* x86_64; 0 on aarch64 */
    uint64_t gicd_base;        /* aarch64; 0 on x86_64 */
    uint64_t gicr_base;        /* aarch64 GICv3 redistributor region;
                                * 0 on x86_64 or when gic_is_v2 */
    uint64_t gicr_size;
    uint64_t gic_cpu_base;     /* aarch64 GICv2 compat CPU interface;
                                * 0 on x86_64 or when GICv3 */
    uint64_t gic_is_v2;        /* 0/1 */
    uint64_t timer_freq_hz;    /* aarch64 generic timer; 0 on x86_64
                                * (LAPIC timer self-calibrates) */
    uint64_t psci_use_hvc;     /* 0=SMC, 1=HVC; aarch64 PSCI only */
};
#define BOOT_PROTO_MD_DATA_WORDS 16 /* sizeof(struct machine_description_data) / 8 --
                                    * keep in sync by hand, same as every
                                    * other wire-shape constant in this
                                    * project (capnp_list.h has no
                                    * compile-time derivation of this). */
#define MACHINE_DESCRIPTION_MAGIC 0x4d414348494e4544ull /* "MACHINED" */

#define BOOT_PROTO_ARCH_X86_64 0
#define BOOT_PROTO_ARCH_AARCH64 1

/* One entry in MachineDescription's core list -- mirrors
 * coreinit/topology.h's topology_core_t exactly (target: the
 * arch-opaque per-core identifier, APIC ID on x86_64/MPIDR affinity on
 * aarch64; enabled: 1 if usable, 0 if present but disabled). Flattened
 * as two consecutive uint64_t words per entry when passed to/read from
 * capnp_list.h's list builder/reader -- there is no struct packing
 * concern here, the wire format is defined in terms of word order, not
 * C struct layout, but this shape matches that order exactly for
 * convenience on both the encode and decode side. */
struct core_entry {
    uint64_t target;
    uint64_t enabled;
};
#define BOOT_PROTO_CORE_ENTRY_WORDS 2

/* ---- InitDescription: real Cap'n Proto object, opaque to saski ----
 *
 * A list of per-architecture init-binary variants, for coreinit's own
 * SMP bring-up to consult if a secondary core turns out to be a
 * different architecture than the BSP -- the original, still-valid
 * reason this object exists (see otos-grub/otos-boot's own READMEs).
 * One data word (a magic for a basic sanity check) and one pointer
 * field, a composite list of InitVariant. */
#define BOOT_PROTO_ID_DATA_WORDS 1
#define INIT_DESCRIPTION_MAGIC 0x494e495444455343ull /* "INITDESC" */

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
