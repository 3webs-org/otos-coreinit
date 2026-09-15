#pragma once

#include "acpi.h"
#include "../include/init_abi.h"

/* Brings up every enabled, non-BSP core found by ACPI discovery: for
 * each one, CLAIMS its assigned region via the memory manager (see
 * mem_mgr.h) before touching it, copies a fresh kernel image into that
 * region, writes the discovery record so the AP's own coreinit can find
 * the BSP-hosted shared services, prepares the real-mode trampoline's
 * per-core parameters, and sends the INIT-SIPI-SIPI sequence via the
 * local APIC's MMIO registers. Success is observed via each AP's own
 * serial output surviving the full real-mode -> protected-mode ->
 * long-mode -> kernel_boot_ap -> coreinit chain -- there is no return
 * value telling you it worked, the same way there's no synchronous
 * confirmation on real hardware either. */
void smp_bring_up_all(const acpi_info_t *acpi, uint8_t bsp_apic_id,
                      const struct init_boot_info *info,
                      capability_t memory_manager, capability_t mutex_manager,
                      uint64_t hardware_mutex_id, uint32_t lapic_calibration_count,
                      capability_t cluster_scheduler, capability_t serial_resource);

/* Reads this core's own APIC ID directly from the local APIC's MMIO ID
 * register -- needed to know which discovered ACPI entry IS the BSP,
 * so it's correctly excluded from bring-up. */
uint8_t smp_read_bsp_apic_id(uint32_t lapic_base);
