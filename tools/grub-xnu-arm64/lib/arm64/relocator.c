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

/* aarch64 relocator: the allocation half only.
 *
 * WHY THIS EXISTS. grub-core/loader/xnu.c -- the portable half of GRUB's
 * xnu loader -- allocates the kernel's heap region through the relocator
 * API: grub_relocator_new, grub_relocator_alloc_chunk_addr,
 * get_virtual_current_address, grub_relocator_unload. GRUB ships that
 * API for i386, x86_64, mips, powerpc, ieee1275 and xen, and NOT for
 * arm64 (grub-core/lib/arm64/ contains only setjmp.S). Without it the
 * portable loader cannot compile on this architecture at all, which is
 * the real reason xnu was x86-only -- not any x86-ness in the loader
 * itself.
 *
 * WHAT THIS IS, STATED PLAINLY. A relocator's job on x86 is to move the
 * running GRUB image out of the way and enter a kernel through a
 * carefully built low-level state. On aarch64 we do not need that: the
 * kernel is entered directly by the arch half (see loader/arm64/xnu.c),
 * with firmware already exited. What the portable loader still needs is
 * only the ALLOCATION service -- "give me a chunk of physical memory at
 * this address, mapped where I can write to it now". That is what this
 * file implements, and nothing else.
 *
 * The chunk list is a simple free-list over allocations made through
 * grub_efi_allocate_pages_real, so a chunk's virtual address (where
 * GRUB writes) and its target physical address (where the kernel will
 * find it) are the same value under EFI's identity mapping at this
 * stage -- which is exactly the property the portable loader assumes
 * when it writes through get_virtual_current_address and hands the
 * numerical address to the kernel. */

#include <grub/dl.h>
#include <grub/mm.h>
#include <grub/misc.h>
#include <grub/relocator.h>
#include <grub/efi/efi.h>
#include <grub/efi/api.h>
#include <grub/cpu/relocator.h>

GRUB_MOD_LICENSE ("GPLv3+");

/* One allocation made through this relocator. The portable loader keeps
 * the relocator alive for the whole boot and unloads it either on
 * failure or never (the kernel takes over); so this list needs to be
 * correct, not fast. */
struct grub_relocator_chunk
{
  struct grub_relocator_chunk *next;
  grub_addr_t target;
  grub_size_t size;
  void *src;
};

struct grub_relocator
{
  struct grub_relocator_chunk *chunks;
};

struct grub_relocator *
grub_relocator_new (void)
{
  struct grub_relocator *rel;

  rel = grub_zalloc (sizeof (*rel));
  if (!rel)
    return NULL;
  return rel;
}

void
grub_relocator_unload (struct grub_relocator *rel)
{
  struct grub_relocator_chunk *c, *next;

  if (!rel)
    return;

  for (c = rel->chunks; c; c = next)
    {
      next = c->next;
      /* The pages themselves are deliberately NOT freed: by the time
       * this runs, on the failure path, the kernel is not going to be
       * entered and firmware will be reset; on the success path this
       * is never called. Freeing them here would also be wrong for the
       * chunks the kernel may already have been told about. */
      grub_free (c);
    }
  grub_free (rel);
}

grub_err_t
grub_relocator_alloc_chunk_addr (struct grub_relocator *rel,
				 grub_relocator_chunk_t *out,
				 grub_addr_t target, grub_size_t size)
{
  struct grub_relocator_chunk *c;
  grub_efi_physical_address_t phys = target;

  if (!rel || size == 0)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "invalid relocation request");

  c = grub_zalloc (sizeof (*c));
  if (!c)
    return grub_errno;

  c->target = target;
  c->size = size;

  /* Allocate exactly at the requested address. GRUB_EFI_ALLOCATE_ADDRESS
   * with a page-aligned target is the "MAP_FIXED" of EFI; a failure here
   * is a real failure (something else owns that memory), not something
   * to paper over by allocating elsewhere -- the portable loader has
   * already computed target addresses that its own kernel expects. */
  if (grub_efi_allocate_pages_real (target,
				    (size + 4095) / 4096,
				    GRUB_EFI_ALLOCATE_ADDRESS,
				    GRUB_EFI_LOADER_DATA) == 0)
    {
      grub_free (c);
      return grub_error (GRUB_ERR_OUT_OF_MEMORY,
			 "cannot allocate 0x%llx bytes at 0x%llx",
			 (unsigned long long) size,
			 (unsigned long long) target);
    }
  (void) phys;

  /* Under EFI at this stage the mapping is identity and the pages are
   * directly writable, so the address GRUB writes to IS the physical
   * address the kernel will later be told about. */
  c->src = (void *) (grub_addr_t) target;

  c->next = rel->chunks;
  rel->chunks = c;

  *out = (grub_relocator_chunk_t) c;
  return GRUB_ERR_NONE;
}

void *
get_virtual_current_address (grub_relocator_chunk_t ch)
{
  return ch->src;
}

grub_addr_t
get_physical_target_address (grub_relocator_chunk_t ch)
{
  return ch->target;
}

/* The faithful counterpart of i386's grub_relocator32_boot.
 *
 * i386 does three things here: exits Boot Services (unless the caller
 * already has), writes the final memory map to the firmware's virtual
 * address map when the EFI variant is used, and jumps through a state
 * describing the target's registers. The aarch64 version keeps that
 * structure and drops only what has no aarch64 meaning: there is no
 * SetVirtualAddressMap step (the kernel is entered with the MMU off and
 * never uses EFI runtime services), and the register state carries the
 * ARM64 boot protocol's argument registers instead of eip/eax/esp/ebp. */
grub_err_t
grub_relocator64_boot (struct grub_relocator *rel,
		       struct grub_relocator64_state state,
		       int avoid_efi_boot_services)
{
  (void) rel;

  if (avoid_efi_boot_services == 0)
    {
      grub_efi_uintn_t size = 0;
      grub_efi_uintn_t key = 0;
      grub_efi_uintn_t desc_size = 0;
      grub_efi_uint32_t desc_version = 0;
      grub_efi_boot_services_t *b;

      b = grub_efi_system_table->boot_services;

      /* The key must come from the CURRENT map: any allocation between
       * taking the map and calling ExitBootServices invalidates it. */
      grub_efi_get_memory_map (&size, NULL, &key, &desc_size, &desc_version);

      if (b->exit_boot_services (grub_efi_image_handle, key)
	  != GRUB_EFI_SUCCESS)
	return grub_error (GRUB_ERR_BAD_OS,
			   "ExitBootServices failed; firmware still owns the machine");
    }

  /* Never returns. */
  grub_relocator64_boot_asm (&state);
}

grub_err_t
grub_relocator_alloc_chunk_align (struct grub_relocator *rel,
				  grub_relocator_chunk_t *out,
				  grub_phys_addr_t min_addr,
				  grub_phys_addr_t max_addr,
				  grub_size_t size, grub_size_t align,
				  int preference,
				  int avoid_efi_boot_services)
{
  grub_addr_t target;

  (void) preference;
  (void) avoid_efi_boot_services;

  /* aarch64 needs no preference ordering: any page-aligned address in
   * [min_addr, max_addr) that is free will do, and the caller's own
   * alignment requirement is satisfied by rounding min_addr up. */
  if (align == 0)
    align = 4096;

  target = ALIGN_UP (min_addr, align);
  if (target + size > max_addr)
    return grub_error (GRUB_ERR_OUT_OF_MEMORY,
		       "no room in [0x%llx, 0x%llx) for 0x%llx bytes",
		       (unsigned long long) min_addr,
		       (unsigned long long) max_addr,
		       (unsigned long long) size);

  return grub_relocator_alloc_chunk_addr (rel, out, target, size);
}
