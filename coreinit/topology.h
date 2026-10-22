/* Architecture-neutral core discovery and identification.
 *
 * WHY THIS EXISTS: different architectures discover cores from different
 * sources and identify them in different namespaces. x86_64 reads the
 * ACPI MADT (via an RSDP pointer) and names cores by APIC ID; aarch64
 * reads a device tree (via a DTB pointer) and names cores by MPIDR
 * affinity. Code above this seam (smp.c, scheduler.c) must not care
 * which -- it works with topology_t, an arch-opaque list of per-core
 * identifiers, and calls exactly one <stem>_<arch>.c implementation
 * (topology_discover in acpi_x86_64.c on x86_64, in fdt_aarch64.c on
 * aarch64) to fill it in.
 *
 * CONVENTION: follows the same pattern arch_mmu.h and cross_core_signal.h
 * already establish -- a small, neutral surface in this header, all
 * architecture-specific detail in exactly one <stem>_<arch>.c file per
 * supported architecture. The neutral caller (smp.c) includes only this
 * header and links against whichever _<arch>.c the build selected. */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include "../include/init_abi.h"

#define TOPOLOGY_MAX_CORES 64

typedef struct {
    uint64_t target;   /* Arch-opaque per-core identifier: APIC ID on x86_64,
                        * MPIDR affinity on aarch64. Used by the scheduler
                        * for affinity and by cross_core_signal_send as the
                        * target argument -- both treat it as an opaque key,
                        * never interpreting its internal structure. */
    uint8_t enabled;   /* 1 if this core is usable, 0 if present but disabled
                        * (e.g. ACPI hotplug placeholder, device-tree "disabled"
                        * status property). Only enabled cores are brought up. */
} topology_core_t;

typedef struct {
    topology_core_t cores[TOPOLOGY_MAX_CORES];
    size_t count;      /* Total cores found (enabled + disabled). Loop bound
                        * for iterating cores[]; check cores[i].enabled to
                        * distinguish usable from placeholder entries. */

    /* Arch-specific discovery results a caller may still need, opaque to
     * arch-neutral code. Each <stem>_<arch>.c interprets its own slots:
     * x86_64 uses [0] for the LAPIC MMIO base physical address (needed
     * later for INIT-SIPI-SIPI and timer calibration), [1..3] reserved.
     * aarch64 may use [0] for GIC distributor base, etc. -- the point is
     * that smp.c and lapic_timer.c can retrieve what they need WITHOUT
     * re-parsing or re-including the arch-specific discovery header. */
    uint64_t arch_opaque[4];
} topology_t;

/* Discovers all cores the architecture's standard mechanism reports.
 * Returns 1 on success (out->count > 0, out->cores[] filled in), 0 if
 * discovery failed (corrupt/missing tables, checksum failure, etc.).
 *
 * arch_hint: an architecture-specific pointer to the discovery root:
 *   - x86_64: the ACPI RSDP physical address (from init_boot_info's
 *     early_rsdp_phys field, found by boot.S before the kernel's own
 *     page tables replaced GRUB/OVMF's). Pass 0 to fall back to a
 *     legacy-BIOS scan (EBDA + 0xE0000-0x100000), though that range
 *     is unreliable on EFI-only boots -- always supply a nonzero value
 *     when available.
 *   - aarch64: the device tree blob (DTB) physical address, handed off
 *     by the bootloader in a standard ARM boot protocol register.
 *
 * Implemented in acpi_x86_64.c on x86_64, fdt_aarch64.c on aarch64. */
int topology_discover(topology_t *out, uint64_t arch_hint);

/* The machine-description hint topology_discover wants, extracted from the
 * kernel's own boot handoff.
 *
 * THE ARCHITECTURAL POINT, and the reason this function exists rather than
 * each caller reading init_boot_info directly:
 *
 * The boot CONVENTION is uniform across every standard platform. One
 * kernel binary carries every standard port, and every one of them is
 * entered the same way -- the kernel builds one init_boot_info and hands
 * it to init. That uniformity is the whole reason this boot protocol was
 * chosen: the handoff shape does not change per port, so neither the
 * kernel's handoff code nor init's entry code needs a per-platform
 * variant. Embedded targets are the acknowledged exception, where that
 * convention is not available and a platform-specific entry is expected.
 *
 * What DOES differ per port is the machine description sitting inside that
 * uniform handoff. Two fields carry it, deliberately never one:
 *
 *   x86_64: early_rsdp_phys -- ACPI, reached via the EFI config table.
 *   aarch64: fdt_blob_phys -- a flattened device tree.
 *
 * They are separate because they carry different formats read by
 * different code, and a single overloaded "arch hint" field would invite
 * treating one as the other. Both are opaque uint64_t physical
 * addresses, so picking the wrong one is invisible to the compiler --
 * passing an RSDP to an FDT parser fails at runtime, in the topology
 * backend, far from the line that made the mistake. Choosing here, in
 * exactly one place per architecture, is what turns that silent runtime
 * error into a one-line reviewable decision. */
uint64_t topology_boot_hint(const struct init_boot_info *info);

/* Returns this core's own identifier in the same namespace topology_discover
 * uses (APIC ID on x86_64, MPIDR on aarch64). Needed to know which entry in
 * topology_t.cores[] IS this core, so it's correctly excluded from bring-up
 * (the BSP doesn't INIT-SIPI-SIPI itself) and for scheduler affinity.
 *
 * Reads directly from hardware (LAPIC ID register on x86_64, MPIDR_EL1 on
 * aarch64) -- callable before topology_discover, since serial tagging and
 * early diagnostics need it immediately.
 *
 * Implemented in acpi_x86_64.c on x86_64, psci_aarch64.c on aarch64. */
uint64_t topology_self_id(void);

/* The arch-opaque timer init hint this topology discovered -- x86_64:
 * the LAPIC MMIO base; aarch64: the generic-timer frequency. Each arch's
 * topology backend returns the right arch_opaque[] slot, so the caller
 * never has to know which slot means what. */
uint64_t topology_timer_hint(const topology_t *t);

/* The CLUSTER_ARCH_* value (see cluster_scheduler.h) this node joins the
 * cluster scheduler as. */
uint64_t topology_cluster_arch(void);
