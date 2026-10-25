/* x86_64 backend for core_state.h. Everything x86-specific about storing
 * this core's own core_state pointer lives in this one file -- porting to
 * another architecture means writing a new file matching core_state.h's
 * contract, not touching anything that includes that header. */
#include "core_state.h"

/* IA32_GS_BASE: a per-core MSR with no architectural meaning forced on it
 * here -- this never loads the GS segment or does %gs-relative memory
 * access, it's used purely as scratch storage for this core's own
 * pointer, read back with the matching rdmsr. */
#define IA32_GS_BASE 0xC0000101u

static inline void wrmsr(uint32_t msr, uint64_t value)
{
    uint32_t lo = (uint32_t)value;
    uint32_t hi = (uint32_t)(value >> 32);
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

void core_state_set(struct core_state *cs)
{
    wrmsr(IA32_GS_BASE, (uint64_t)(uintptr_t)cs);
}

struct core_state *core_state_get(void)
{
    return (struct core_state *)(uintptr_t)rdmsr(IA32_GS_BASE);
}
