#pragma once

#include "../include/securinit_abi.h"

#if defined(__x86_64__)
/* Trusted wrapper owns CPL3 page grant, transition, INT80/INT3 handlers,
 * and denial of direct Admin memory access. coreinit only installs IDT/TSS
 * hardware routes through a factory-created router object. */
int securinit_x86_isolation_probe(const struct securinit_boot_info *info,
                                  uint64_t privileged_addr,
                                  capability_t granted_gate);
#endif
