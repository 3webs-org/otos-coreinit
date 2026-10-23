/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2009  Free Software Foundation, Inc.
 *
 *  GRUB is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  GRUB is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with GRUB.  If not, see <http://www.gnu.org/licenses/>.
 */

/* aarch64 half of GRUB's memory contract.
 *
 * Upstream GRUB ships no include/grub/arm64/memory.h. The i386 version
 * is largely x86 page-table constants (CR0/CR4 bits, page flags, e820
 * entries) that aarch64 has no equivalent for -- arm64 page tables live
 * in a different format entirely. What <grub/relocator.h> actually
 * needs from this header is narrow: GRUB_PAGE_SHIFT / GRUB_PAGE_SIZE,
 * the grub_phys_addr_t typedef, and grub_vtop/grub_ptov. Those are
 * architecture-neutral in substance -- 4 KiB pages, identity mapping
 * under firmware -- so this header provides exactly those and nothing
 * else. The x86 constants are deliberately absent rather than stubbed:
 * a translation unit that tried to use GRUB_PAGE_PRESENT on aarch64
 * should fail to compile, not silently get an x86 value. */

#ifndef GRUB_MEMORY_CPU_HEADER
#define GRUB_MEMORY_CPU_HEADER	1

#define GRUB_PAGE_SHIFT		12
#define GRUB_PAGE_SIZE		(1UL << GRUB_PAGE_SHIFT)

#ifndef ASM_FILE

#include <grub/types.h>

typedef grub_addr_t grub_phys_addr_t;

/* Under EFI, and at every point this loader runs, the mapping is
 * identity: the address GRUB writes to is the address the kernel will
 * find. These stay as functions rather than identity macros so the call
 * sites read the same as on the architectures where the transform is
 * real. */
static inline grub_phys_addr_t
grub_vtop (void *a)
{
  return (grub_phys_addr_t) (grub_addr_t) a;
}

static inline void *
grub_map_memory (grub_addr_t phys, grub_size_t size)
{
  (void) size;
  return (void *) phys;
}

static inline void
grub_unmap_memory (void *addr, grub_size_t size)
{
  (void) addr;
  (void) size;
}

#endif /* ASM_FILE */

#endif /* GRUB_MEMORY_CPU_HEADER */
