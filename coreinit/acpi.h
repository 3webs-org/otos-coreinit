#pragma once

#include <stddef.h>
#include <stdint.h>

#define ACPI_MAX_CPUS 64

typedef struct {
    uint8_t apic_id;
    uint8_t enabled; /* MADT LAPIC flags bit 0 -- see acpi.c's own note on
                      * why a present-but-disabled entry (real on QEMU,
                      * used for hotplug support) must not be treated as
                      * a usable core */
} acpi_cpu_t;

typedef struct {
    acpi_cpu_t cpus[ACPI_MAX_CPUS];
    size_t cpu_count;
    uint32_t local_apic_address; /* physical address of the LAPIC MMIO
                                  * region, from the MADT header -- needed
                                  * later for the actual INIT-SIPI-SIPI
                                  * sequence, not used by discovery itself */
} acpi_info_t;

/* Finds the RSDP, validates it, walks the RSDT or XSDT (whichever the
 * RSDP's revision indicates) to find the MADT, validates that too, and
 * fills in `out` with every Processor Local APIC entry found. Returns 1
 * on success, 0 if the RSDP or MADT couldn't be found or failed its
 * checksum -- ACPI's own attestation mechanism catches a corrupt or
 * absent table before we'd otherwise misread it.
 *
 * early_rsdp_phys: pass 0 to have this function do its own legacy-BIOS
 * scan (EBDA, then 0xE0000-0x100000) as a fallback -- boot.S normally
 * always supplies a nonzero value here (found by its own, earlier
 * 32-bit-mode scan; see that file's own comment), since this project's
 * memory ownership starts well above where a legacy scan alone would
 * reliably find it: the RSDP sits at a firmware-chosen, run-to-run-
 * variable low address this function's own legacy-BIOS ranges don't
 * cover, so boot.S finds it itself, before this kernel's own identity
 * map replaces GRUB/OVMF's page tables, and hands the address across
 * via init_boot_info's early_rsdp_phys field -- see that field's
 * own comment for the full story. A nonzero value handed in here is
 * still re-validated by checksum, not trusted blindly. */
int acpi_discover(acpi_info_t *out, uint64_t early_rsdp_phys);
