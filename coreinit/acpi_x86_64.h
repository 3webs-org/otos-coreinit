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

/* x86_64-internal ACPI structures and parsing -- now wrapped by the
 * arch-neutral topology.h interface (topology_discover calls acpi_discover
 * internally). Direct use of acpi_info_t is still needed by lapic_timer.c
 * and job_timer_bringup (which need the LAPIC MMIO base), so this header
 * remains, but acpi_discover itself is now static in acpi_x86_64.c. */
