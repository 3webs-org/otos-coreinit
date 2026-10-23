#include "stage_loader.h"
#include <stdint.h>

#define MH_MAGIC_64 0xfeedfacfu
#define LC_SEGMENT_64 0x19u
#define LC_MAIN 0x80000028u
#if defined(__aarch64__)
#define CPU_TYPE_NATIVE 0x0100000cu
#else
#define CPU_TYPE_NATIVE 0x01000007u
#endif

typedef struct {
    uint32_t magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds,
             flags, reserved;
} mach_header_64_t;

typedef struct {
    uint32_t cmd, cmdsize;
} load_command_t;

typedef struct {
    uint32_t cmd, cmdsize;
    char segname[16];
    uint64_t vmaddr, vmsize, fileoff, filesize;
    uint32_t maxprot, initprot, nsects, flags;
} segment_command_64_t;

typedef struct {
    uint32_t cmd, cmdsize;
    uint64_t entryoff, stacksize;
} entry_point_command_t;

static int is_pagezero(const char name[16])
{
    const char expected[] = "__PAGEZERO";
    for (unsigned i = 0; i < sizeof(expected); i++) {
        if (name[i] != expected[i]) {
            return 0;
        }
    }
    return 1;
}

#if defined(__aarch64__)
static void sync_executable(uint64_t start, uint64_t end)
{
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    uint64_t dline = 4ull << ((ctr >> 16) & 0xfu);
    uint64_t iline = 4ull << (ctr & 0xfu);
    for (uint64_t addr = start & ~(dline - 1); addr < end; addr += dline) {
        __asm__ volatile("dc cvau, %0" :: "r"(addr) : "memory");
    }
    __asm__ volatile("dsb ish" ::: "memory");
    for (uint64_t addr = start & ~(iline - 1); addr < end; addr += iline) {
        __asm__ volatile("ic ivau, %0" :: "r"(addr) : "memory");
    }
    __asm__ volatile("dsb ish\n\tisb" ::: "memory");
}
#endif

uint64_t stage_load_macho(const void *image, size_t image_len,
                          uint64_t target_base, uint64_t reserved_size)
{
    if (!image || image_len < sizeof(mach_header_64_t)) {
        return 0;
    }
    const uint8_t *bytes = (const uint8_t *)image;
    const mach_header_64_t *header = (const mach_header_64_t *)image;
    if (header->magic != MH_MAGIC_64 || header->cputype != CPU_TYPE_NATIVE ||
        header->sizeofcmds > image_len - sizeof(*header)) {
        return 0;
    }
    const uint8_t *p = bytes + sizeof(*header);
    const uint8_t *end = p + header->sizeofcmds;
    uint64_t entryoff = 0, entry = 0;
#if defined(__aarch64__)
    uint64_t code_start = UINT64_MAX, code_end = 0;
#endif
    int have_entry = 0;
    for (uint32_t i = 0; i < header->ncmds; i++) {
        if ((size_t)(end - p) < sizeof(load_command_t)) {
            return 0;
        }
        const load_command_t *command = (const load_command_t *)p;
        if (command->cmdsize < sizeof(*command) || command->cmdsize > (size_t)(end - p)) {
            return 0;
        }
        if (command->cmd == LC_SEGMENT_64) {
            if (command->cmdsize < sizeof(segment_command_64_t)) {
                return 0;
            }
            const segment_command_64_t *seg = (const segment_command_64_t *)p;
            if (!is_pagezero(seg->segname)) {
                if (seg->vmaddr > reserved_size ||
                    seg->vmsize > reserved_size - seg->vmaddr ||
                    seg->filesize > seg->vmsize ||
                    seg->fileoff > image_len ||
                    seg->filesize > image_len - seg->fileoff) {
                    return 0;
                }
                uint8_t *dest = (uint8_t *)(uintptr_t)(target_base + seg->vmaddr);
                const uint8_t *src = bytes + seg->fileoff;
                for (uint64_t b = 0; b < seg->filesize; b++) {
                    dest[b] = src[b];
                }
                for (uint64_t b = seg->filesize; b < seg->vmsize; b++) {
                    dest[b] = 0;
                }
#if defined(__aarch64__)
                if (seg->initprot & 4u) {
                    uint64_t start = target_base + seg->vmaddr;
                    if (start < code_start) code_start = start;
                    if (start + seg->filesize > code_end) code_end = start + seg->filesize;
                }
#endif
                if (have_entry && entryoff >= seg->fileoff &&
                    entryoff - seg->fileoff < seg->filesize) {
                    entry = target_base + seg->vmaddr + entryoff - seg->fileoff;
                }
            }
        } else if (command->cmd == LC_MAIN) {
            if (command->cmdsize < sizeof(entry_point_command_t)) {
                return 0;
            }
            entryoff = ((const entry_point_command_t *)p)->entryoff;
            have_entry = 1;
        }
        p += command->cmdsize;
    }
    /* LC_MAIN can appear after __TEXT: resolve its file offset in a
     * second pass, as with the kernel's own independent Mach-O loader. */
    if (have_entry && !entry) {
        p = bytes + sizeof(*header);
        for (uint32_t i = 0; i < header->ncmds; i++) {
            const load_command_t *command = (const load_command_t *)p;
            if (command->cmd == LC_SEGMENT_64) {
                const segment_command_64_t *seg = (const segment_command_64_t *)p;
                if (!is_pagezero(seg->segname) && entryoff >= seg->fileoff &&
                    entryoff - seg->fileoff < seg->filesize) {
                    entry = target_base + seg->vmaddr + entryoff - seg->fileoff;
                    break;
                }
            }
            p += command->cmdsize;
        }
    }
#if defined(__aarch64__)
    if (entry && code_start < code_end) {
        sync_executable(code_start, code_end);
    }
#endif
    return entry;
}
