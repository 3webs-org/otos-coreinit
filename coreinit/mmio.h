#pragma once

#include <stdint.h>

/* Deliberately explicit, volatile-qualified accessors for anything
 * crossing a core boundary -- another core's assigned region, its
 * trailing mailbox, or a real MMIO device register are all treated the
 * same way here: something to read/write through a narrow, explicit
 * interface, never an ordinary C variable implicitly shared via
 * whatever cache-coherence the current hardware happens to provide.
 * This is what keeps the door open for the transport underneath to
 * become something other than coherent shared memory later (a real
 * network leg, a non-coherent interconnect) without every call site
 * needing to change -- only what's behind these functions would. */

static inline uint64_t mmio_read64(const volatile void *addr)
{
    return *(const volatile uint64_t *)addr;
}

static inline void mmio_write64(volatile void *addr, uint64_t val)
{
    *(volatile uint64_t *)addr = val;
}

static inline uint32_t mmio_read32(const volatile void *addr)
{
    return *(const volatile uint32_t *)addr;
}

static inline void mmio_write32(volatile void *addr, uint32_t val)
{
    *(volatile uint32_t *)addr = val;
}
