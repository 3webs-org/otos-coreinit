#pragma once

#include "topology.h"
#include "../include/init_abi.h"

/* Brings up every enabled, non-BSP core found by topology discovery: for
 * each one, CLAIMS its assigned region via the memory manager (see
 * mem_mgr.h) before touching it, copies a fresh kernel image into that
 * region, writes the discovery record so the AP's own coreinit can find
 * the BSP-hosted shared services, prepares the real-mode trampoline's
 * per-core parameters, and sends the INIT-SIPI-SIPI sequence via the
 * local APIC's MMIO registers (on x86_64) or PSCI CPU_ON (on aarch64).
 * Success is observed via each AP's own serial output surviving the full
 * real-mode -> protected-mode -> long-mode -> kernel_boot_ap -> coreinit
 * chain -- there is no return value telling you it worked, the same way
 * there's no synchronous confirmation on real hardware either. */
void smp_bring_up_all(const topology_t *topo,
                      const struct init_boot_info *info,
                      capability_t memory_manager, capability_t mutex_manager,
                      uint64_t hardware_mutex_id, uint32_t lapic_calibration_count,
                      capability_t cluster_scheduler, capability_t serial_resource);
