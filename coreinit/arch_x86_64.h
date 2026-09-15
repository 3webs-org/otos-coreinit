#pragma once

#include <stdint.h>

/* coreinit's own copy of the amd64 inline-assembly primitives -- not a
 * #include of the kernel's kernel/arch_x86_64.h.
 *
 * The kernel's copy gates every primitive on requires_thread_token(mode64_token)
 * because something in the kernel's own translation unit genuinely calls
 * enter_long_mode() first, and the checker can trace that call. coreinit
 * has no such call anywhere in its own source at all: the 32-to-64-bit
 * transition happened in the kernel's entirely separate compilation, and
 * coreinit's very existence as a running program is proof it already
 * happened -- but that's an axiom about the boot chain's construction,
 * not a fact derivable from any call this translation unit can see. The
 * correct way to express "this whole program starts with X already
 * true" is a checkBeginFunction-based axiom mechanism (mirroring
 * MemoryContractChecker's own fields_established pattern), which
 * calgebra-lints does not have yet -- tracked as follow-up work, not
 * worked around with an annotation that wouldn't actually check
 * anything real. Until then, coreinit's copy of these primitives is
 * plain and ungated, same code, different (currently unstated) trust
 * boundary. */

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outw(uint16_t port, uint16_t val)
{
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16_t inw(uint16_t port)
{
    uint16_t ret;
    __asm__ volatile("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outl(uint16_t port, uint32_t val)
{
    __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port)
{
    uint32_t ret;
    __asm__ volatile("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void halt_forever(void)
{
    for (;;) {
        __asm__ volatile("hlt");
    }
}
