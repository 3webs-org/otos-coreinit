#pragma once

#include <stddef.h>
#include <stdint.h>

/* Load an independently linked, native-architecture Mach-O into a region
 * claimed through the memory-manager object. target_base is a slide atop
 * the image's own vmaddrs; returns the entry address or zero on failure.
 * This loader is deliberately outside saski: stage policy belongs to init. */
uint64_t stage_load_macho(const void *image, size_t image_len,
                          uint64_t target_base, uint64_t reserved_size);
