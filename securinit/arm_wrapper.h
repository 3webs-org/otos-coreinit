#pragma once

#include "../include/securinit_abi.h"

#if defined(__aarch64__)
/* Trusted wrapper object owns the guest's EL0 mapping, entry/exit and
 * lower-EL synchronous policy. The existing coreinit vector only saves a
 * frame and forwards exceptions to this registered callback. */
int securinit_arm_isolation_probe(const struct securinit_boot_info *info,
                                  uint64_t privileged_addr,
                                  capability_t granted_gate,
                                  uint64_t value, uint64_t *out_forwarded);
#endif
