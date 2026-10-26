#pragma once

#include <stddef.h>
#include <stdint.h>

/* A minimal, read-only Flattened Device Tree (DTB) walker -- entirely
 * architecture-neutral, no dependency on anything x86-specific anywhere in
 * this file or fdt.c. Exists so this project can discover real hardware facts
 * (how much RAM actually exists and where, where the interrupt controller and
 * the console UART live, how many CPUs there are) at boot instead of asserting
 * fixed-address constants tuned by hand for one specific platform's quirks --
 * see include/smp_layout.h's own, honest admission that its addresses (16 MiB,
 * 24 MiB, 64 MiB...) are workarounds for THIS project's current
 * x86/QEMU/OVMF/GRUB-xnu boot path specifically, not portable minimums.
 *
 * Deliberately does NOT depend on libfdt or any other existing implementation
 * -- this project's own freestanding, no-libc build environment (see
 * kernel/macho.c's own from-scratch Mach-O parser for the same reasoning)
 * makes pulling in an external library more friction than writing the few
 * hundred lines this format actually needs. Read-only: nothing here ever
 * modifies the blob.
 *
 * NODE OFFSETS. Functions that take or return a "node offset" use a byte
 * offset from the start of the blob, the same convention libfdt uses, so that
 * an offset is a stable identifier across calls and can be passed around
 * cheaply. FDT_NODE_ROOT (0) is not valid as an offset in this scheme -- the
 * root node's structure-block offset is what fdt_path_offset("/") returns, and
 * every other path resolves through it. Functions return FDT_ERR_NODE (-1)
 * where an offset is expected and no node matches.
 *
 * UNTRUSTED INPUT. On an ARM boot the blob arrives from firmware in a
 * register, and is therefore attacker-adjacent: every walk in fdt.c
 * bounds-checks each advance against the structure block's declared extent
 * before using a byte. A malformed or truncated blob yields "not found" or
 * "0 ranges", never an out-of-bounds read. Callers should still call
 * fdt_valid first, but nothing here requires it to be safe.
 *
 * Devicetree Specification (the authoritative source for this format):
 * https://www.devicetree.org/specifications/ */

/* A single (base, size) range in bytes -- a memory bank, a region that must
 * not be touched (from the FDT's own reservation block, or a
 * /reserved-memory child), or a register window translated into CPU physical
 * addresses. */
typedef struct {
    uint64_t base;
    uint64_t size;
} fdt_range_t;

#define FDT_ERR_NODE (-1)

/* Validates the blob's own header (magic number, and that its declared
 * totalsize doesn't claim to extend past blob_size if that's known -- pass 0
 * if the caller genuinely doesn't know the blob's own size yet and just wants
 * the magic checked). Also requires the structure and strings block offsets
 * and sizes to lie inside totalsize, because every later walk depends on those
 * being sane and checking them once here is cheaper than checking them at
 * every use. Returns 1 if this looks like a real FDT, 0 otherwise -- every
 * other function in this file assumes this has already been checked, and none
 * of them re-validate. */
int fdt_valid(const void *blob, uint64_t blob_size);

/* Fills `out` with up to `max` (base, size) pairs, one per byte range found
 * across every top-level "/memory" (or "/memory@...") node's own "reg"
 * property -- the FDT's own declaration of how much RAM exists and where.
 * Returns the number of ranges actually written (0 if the blob has no memory
 * node at all, which callers should treat as "discovery failed", not "no
 * memory exists").
 *
 * This alone does not mean every byte in these ranges is free to use -- see
 * fdt_reserved_ranges for what must be excluded first. */
size_t fdt_memory_ranges(const void *blob, fdt_range_t *out, size_t max);

/* Fills `out` with up to `max` (base, size) pairs from the FDT's own
 * fixed-format memory reservation block (distinct from any /reserved-memory
 * node in the structure block proper -- this is the simpler, older mechanism
 * every FDT-consuming bootloader is expected to honor unconditionally,
 * typically used for wherever the DTB blob itself, or an initrd/initramfs,
 * currently lives). Returns the number of ranges actually written. */
size_t fdt_reserved_ranges(const void *blob, fdt_range_t *out, size_t max);

/* Enumerates a node's children with a caller-held cursor, which is what a
 * caller walking /cpus or /intc actually wants: initialise *cursor to 0 and
 * call until it returns 0. The callee advances the cursor. This is a wrapper
 * over fdt_first_child/fdt_next_sibling rather than a second walk, so there is
 * one implementation and no chance of the two disagreeing. */
int fdt_next_child(const void *blob, int parent_offset, int *cursor);

/* Returns 1 if the node is present and not marked disabled, 0 otherwise.
 * A node is enabled unless its own "status" property says otherwise: "okay"
 * and "ok" mean enabled, an absent status means enabled, anything else means
 * disabled. Enumerating a disabled device or CPU is worse than not finding it,
 * because it yields a plausible count that does not exist. */
int fdt_is_enabled(const void *blob, int node_offset);

/* Reads a node's #address-cells / #size-cells WITH INHERITANCE from the nearest
 * ancestor that declares them, since that is how a reg entry's width is
 * determined. Exposed rather than re-derived at call sites: getting the
 * inheritance wrong is the most common DTB bug and it silently misreads every
 * reg in the tree instead of failing. Either out-pointer may be NULL. */
int fdt_cell_counts(const void *blob, int node_offset, int *out_address_cells,
                    int *out_size_cells);

/* Reads a node's reg entry and translates it to a CPU-visible address,
 * applying every enclosing bus's ranges mapping. Writes base/size (either may
 * be NULL) and returns 1, or 0 when there is no such entry. This is the
 * two-out-parameter form of fdt_reg_to_cpu_addr, which fills an fdt_range_t;
 * both share one implementation. */
/* Reads the cell counts a node DECLARES FOR ITS CHILDREN, i.e. its own
 * #address-cells / #size-cells, with no inheritance. Returns 1 only when at
 * least one of them is present on the node itself; *ac and *sc are undefined
 * when it returns 0, so "declares nothing" is distinguishable from "declares
 * 0" -- which are different answers and conflating them silently mis-sizes
 * every child reg.
 *
 * This is the companion to fdt_cell_counts, and the two answer different
 * questions. fdt_cell_counts(node) gives the counts that size THAT NODE's own
 * reg entries, which per spec come from its parent. fdt_own_cell_counts(node)
 * gives the counts that size its CHILDREN's reg entries. A caller checking that
 * a tree is self-consistent needs the second: asking "/cpus, what do you
 * declare for your children?" has no answer through fdt_cell_counts, because
 * that function reports what /cpus's PARENT declared.
 *
 * Whichever of the two output values the node does not declare is left
 * untouched, so a caller wanting the full effective pair should assign the
 * specification's defaults first and let this function fill in what the node
 * actually declares. */
int fdt_own_cell_counts(const void *blob, int node_offset, int *out_address_cells,
                        int *out_size_cells);

int fdt_reg_to_addr(const void *blob, int node_offset, uint64_t *out_base,
                    uint64_t *out_size);

/* Resolves a full path such as "/soc/serial@1000" to a node offset, or
 * FDT_ERR_NODE if no such node exists. "/" resolves to the root node. The
 * comparison is exact: "serial@1000" does not match "serial@1001", and the
 * unit address in a node's name is part of its identity, per spec. */
int fdt_path_offset(const void *blob, const char *path);

/* Returns a pointer to a property's value bytes and stores its length in
 * *out_len (which may be NULL if the caller doesn't care). Returns NULL if the
 * node or the property does not exist. A property whose length is 0 (a flag
 * property, e.g. "interrupt-controller") returns a valid pointer with
 * *out_len == 0, which is distinct from NULL. The pointer aliases the blob:
 * it stays valid as long as the blob does, and must not be written. */
const void *fdt_getprop(const void *blob, int node_offset, const char *name,
                        uint32_t *out_len);

/* Walks the children of a node. fdt_first_child returns the first child's
 * offset or FDT_ERR_NODE if there are none; fdt_next_sibling continues from a
 * given child. Together they make a small, fixed-cost iterator with no
 * allocation, which is what this project's freestanding environment needs. */
int fdt_first_child(const void *blob, int parent_offset);
int fdt_next_sibling(const void *blob, int parent_offset, int prev_sibling);

/* Returns the offset of the parent of a node, or FDT_ERR_NODE for the root.
 * Needed because reg translation walks upward (see fdt_reg_to_cpu_addr). */
int fdt_parent_offset(const void *blob, int node_offset);

/* Finds the first node whose "compatible" property matches `compatible`.
 *
 * The match is against EVERY string in the property, not just the first: a
 * "compatible" value is a NUL-separated, preference-ordered list (for example
 * "fsl,imx6q-uart", "fsl,imx21-uart", or "arm,pl011", "arm,primecell"),
 * and a driver registered for the second entry is still the right driver. Only
 * matching the first entry is a classic DTB bug, so it is called out here.
 * Returns FDT_ERR_NODE if nothing matches. */
int fdt_node_by_compatible_offset(const void *blob, const char *compatible);

/* Continues the same depth-first compatible search after `previous_offset`.
 * Pass FDT_ERR_NODE for the first match. This is required for buses such as
 * virtio-mmio where many sibling transports share one compatible string but
 * expose different device ids at runtime. */
int fdt_next_node_by_compatible_offset(const void *blob,
                                       const char *compatible,
                                       int previous_offset);

/* Same search, reporting through an out-pointer: returns 1 when found and 0
 * when not, so "found nothing" cannot be confused with a valid offset of 0. */
int fdt_node_by_compatible(const void *blob, const char *compatible,
                           int *out_offset);

/* Reads a node's own "#address-cells"/"#size-cells", which size its children's
 * "reg" entries per spec. The values are inherited from the nearest ancestor
 * that sets them, defaulting to the spec's 2/2 for #address-cells/#size-cells
 * only when nothing up the chain sets them at all. Writing that inheritance
 * out once, here, is the point: re-implementing it at each call site is how
 * DTB readers get subtly wrong cell widths. Either output pointer may be NULL. */
int fdt_parent_cells(const void *blob, int node_offset, uint32_t *addr_cells,
                     uint32_t *size_cells);

/* Translates a node's `index`-th "reg" entry into a CPU physical address,
 * applying every enclosing bus's "ranges" mapping on the way up. Writes the
 * translated (base, size) into *out and returns 1 on success, 0 if the node
 * has no such reg entry.
 *
 * Why this exists at all: on real hardware a peripheral's "reg" is expressed
 * in its PARENT BUS's address space, not the CPU's, so a UART inside a
 * simple-bus typically declares reg = <0x1000 0x1000> while the CPU reaches
 * it at 0x80001000. A reader that skips the translation looks correct on
 * QEMU's -machine virt, whose ranges happen to be identity, and then reads
 * the wrong address on hardware -- which is precisely the class of bug
 * smp_layout.h's own comment warns about. An empty "ranges" property means an
 * identity mapping (the bus maps 1:1 over 32 bits); an absent one means the
 * parent does not map this bus at all, and the address is passed through
 * unchanged rather than invented. */
int fdt_reg_to_cpu_addr(const void *blob, int node_offset, uint32_t index,
                        fdt_range_t *out);
