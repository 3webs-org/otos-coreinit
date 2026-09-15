#pragma once

#include <stddef.h>
#include <stdint.h>

/* A minimal, read-only Flattened Device Tree (DTB) walker -- entirely
 * architecture-neutral, no dependency on anything x86-specific
 * anywhere in this file or fdt.c. Exists so this project can discover
 * real hardware facts (how much RAM actually exists, and where) at
 * boot instead of asserting fixed-address constants tuned by hand for
 * one specific platform's own quirks -- see include/smp_layout.h's
 * own, honest admission that its addresses (16 MiB, 24 MiB, 64 MiB...)
 * are workarounds for THIS project's current x86/QEMU/OVMF/GRUB-xnu
 * boot path specifically, not portable minimums. A target with only a
 * few hundred KiB total RAM (an ESP32-class device, say) can't afford
 * ANY of those numbers; the fix is discovering the real layout, not
 * picking smaller magic constants by hand for every platform in turn.
 *
 * Deliberately does NOT depend on libfdt or any other existing
 * implementation -- this project's own freestanding, no-libc build
 * environment (see kernel/macho.c's own from-scratch Mach-O parser
 * for the same reasoning) makes pulling in an external library more
 * friction than writing the ~150 lines this format actually needs.
 * Read-only: nothing here ever modifies the blob.
 *
 * Devicetree Specification (the authoritative source for this format):
 * https://www.devicetree.org/specifications/ */

/* A single (base, size) range in bytes -- a memory bank, or a region
 * that must not be touched (from the FDT's own reservation block, or
 * a /reserved-memory child). */
typedef struct {
    uint64_t base;
    uint64_t size;
} fdt_range_t;

/* Validates the blob's own header (magic number, and that its
 * declared totalsize doesn't claim to extend past blob_size if that's
 * known -- pass 0 if the caller genuinely doesn't know the blob's own
 * size yet and just wants the magic checked). Returns 1 if this looks
 * like a real FDT, 0 otherwise -- every other function in this file
 * assumes this has already been checked and won't re-validate. */
int fdt_valid(const void *blob, uint64_t blob_size);

/* Fills `out` with up to `max` (base, size) pairs, one per byte range
 * found across every top-level "/memory" (or "/memory@...") node's
 * own "reg" property -- the FDT's own declaration of how much RAM
 * exists and where. Returns the number of ranges actually written
 * (0 if the blob has no memory node at all, which callers should
 * treat as "discovery failed", not "no memory exists").
 *
 * This alone does not mean every byte in these ranges is free to use
 * -- see fdt_reserved_ranges for what must be excluded first. */
size_t fdt_memory_ranges(const void *blob, fdt_range_t *out, size_t max);

/* Fills `out` with up to `max` (base, size) pairs from the FDT's own
 * fixed-format memory reservation block (distinct from any
 * /reserved-memory node in the structure block proper -- this is the
 * simpler, older mechanism every FDT-consuming bootloader is expected
 * to honor unconditionally, typically used for wherever the DTB blob
 * itself, or an initrd/initramfs, currently lives). Returns the
 * number of ranges actually written. */
size_t fdt_reserved_ranges(const void *blob, fdt_range_t *out, size_t max);
