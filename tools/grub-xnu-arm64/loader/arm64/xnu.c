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

/* aarch64 implementation of the xnu loader's arch contract.
 *
 * WHAT THIS FILE IS. grub-core/loader/xnu.c is the portable half of
 * GRUB's xnu loader -- 1556 lines that build the device tree, resolve
 * kexts, write device-properties, and drive grub_loader. It was gated
 * to x86 in Makefile.core.def purely because its arch half only existed
 * for x86, not because the portable half is x86-specific: it uses
 * grub_xnu_ptr_t zero times. This file is the arm64 arch half, so the
 * same portable loader can run on both.
 *
 * WHAT IT PROVIDES. The contract declared in include/grub/arm64/xnu.h:
 * grub_cpu_xnu_init/fini/unload, the devprop helpers, grub_xnu_cmdline,
 * grub_xnu_entry_point, grub_xnu_arg1, and grub_xnu_boot -- the function
 * grub_loader calls to actually leave firmware and enter the kernel.
 *
 * THE ENTRY, WHICH IS WHERE THIS DIVERGES FROM x86. On x86_64
 * grub_xnu_boot builds the boot-parameter block, calls
 * grub_autoefi_set_virtual_address_map and grub_relocator32_boot, and
 * lands in the kernel through a 32-bit relocator state carrying eip/eax/
 * esp/ebp. None of that machinery exists for aarch64: GRUB ships no
 * arm64 relocator at all (grub-core/lib/arm64/ holds only setjmp.S).
 *
 * The aarch64 analogue that needs no relocator: ExitBootServices,
 * then branch to the kernel's entry with x0 holding the physical
 * address of the boot-parameter block. One register, one pointer, one
 * structure -- the same shape the x86 path has, expressed in the
 * calling convention this architecture actually has. That is what
 * grub_xnu_boot does below.
 *
 * DELIBERATELY NOT IMPLEMENTED HERE: the devprop device-property
 * machinery. On x86 it exists to hand macOS properties describing PCI
 * devices (bus/slot/function, ACPI paths) that macOS cannot discover
 * itself under Apple's firmware. The consumer of this port is not
 * macOS; it is a kernel that takes init_boot_info and reads its machine
 * description from the device tree or ACPI. Carrying 900 lines of
 * dead property-construction code to satisfy a linker would be worse
 * than saying so: the functions exist, they are honest no-ops or
 * unsupported-error returns, and the next person knows exactly where
 * the line is. */

#include <grub/dl.h>
#include <grub/misc.h>
#include <grub/mm.h>
#include <grub/env.h>
#include <grub/command.h>
#include <grub/i18n.h>
#include <grub/loader.h>
#include <grub/efi/efi.h>
#include <grub/efi/api.h>
#include <grub/cpu/xnu.h>
#include <grub/xnu.h>

GRUB_MOD_LICENSE ("GPLv3+");

/* The portable loader writes these; grub_xnu_boot reads them. */
grub_xnu_ptr_t grub_xnu_entry_point;
grub_xnu_ptr_t grub_xnu_arg1;
char grub_xnu_cmdline[1024];

 /* Set by the portable loader (kernel_xnu64) via grub_xnu_is_64bit. */
extern int grub_xnu_is_64bit;

/* Where the boot-parameter block was placed, and how big it is. The
 * portable half allocates it through grub_xnu_heap_alloc, which records
 * the target physical address in grub_xnu_heap_target_start. */
extern grub_addr_t grub_xnu_heap_target_start;
extern grub_uint64_t grub_xnu_heap_size;

static grub_efi_uintn_t
efi_mmap_size (grub_efi_uintn_t *out_desc_size,
               grub_efi_uint32_t *out_desc_version)
{
  grub_efi_uintn_t size = 0;
  grub_efi_uintn_t key;
  grub_efi_get_memory_map (&size, NULL, &key, out_desc_size,
                           out_desc_version);
  return size;
}

/* Leaving firmware is not optional: the kernel we enter is not an EFI
 * application, and once we have taken the memory map and written the
 * boot-parameter block there is nothing left that firmware may be
 * allowed to touch. */
static grub_err_t
xnu_leave_firmware_and_enter (grub_addr_t bootparams_target,
                              grub_addr_t stack_top)
{
  /* State for the relocator, exactly as loader/i386/xnu.c fills in its
   * own: the entry point, the first argument, and the stack. On i386
   * those are eip/eax/esp; here they are pc/x0/sp -- the ARM64 boot
   * protocol's own argument register, so the kernel side reads the
   * boot-parameter block from x0 on either architecture's convention. */
  struct grub_relocator64_state state = {0};

  state.x0 = (grub_uint64_t) bootparams_target;
  state.pc = (grub_uint64_t) grub_xnu_entry_point;
  state.sp = (grub_uint64_t) stack_top;

  return grub_relocator64_boot (grub_xnu_relocator, state, 0);
}

grub_err_t
grub_xnu_boot (void)
{
  struct grub_xnu_boot_params_v2 *bp;
  grub_addr_t bootparams_target;
  grub_addr_t devtree_target = 0;
  grub_uint64_t devtreelen = 0;
  grub_efi_uintn_t mmap_size = 0, desc_size = 0;
  grub_efi_uint32_t desc_version = 0;
  void *memory_map = NULL;
  void *map_target = NULL;
  grub_addr_t stack_top;
  grub_efi_uintn_t key = 0;

  /* The portable loader already allocated the boot-parameter block in
   * the kernel's heap region and recorded where. */
  bootparams_target = grub_xnu_heap_target_start;
  bp = (struct grub_xnu_boot_params_v2 *) (grub_addr_t) bootparams_target;

  grub_memset (bp, 0, sizeof (*bp));

  bp->verminor = GRUB_XNU_BOOTARGSV2_VERMINOR;
  bp->vermajor = GRUB_XNU_BOOTARGSV2_VERMAJOR;
  bp->efi_uintnbits = (grub_uint8_t) (sizeof (grub_efi_uintn_t) * 8);

  grub_memcpy (bp->common.cmdline, grub_xnu_cmdline,
               sizeof (bp->common.cmdline));

  /* The memory map is taken here rather than through the x86 path's
   * grub_autoefi_prepare, which is x86-only. Its lifetime ends at
   * ExitBootServices, so it must be copied into the kernel's own
   * region for the kernel to see it. */
  grub_efi_get_memory_map (&mmap_size, NULL, &key, &desc_size,
                           &desc_version);
  if (mmap_size)
    {
      memory_map = grub_malloc (mmap_size);
      if (!memory_map)
        return grub_errno;
      if (!grub_efi_get_memory_map (&mmap_size, memory_map, &key, &desc_size,
                                    &desc_version))
        {
          grub_free (memory_map);
          return grub_error (GRUB_ERR_BAD_OS, "could not read the EFI memory map");
        }
      {
        void *heap_src = NULL;
        grub_addr_t heap_target = 0;
        if (grub_xnu_heap_malloc ((int) mmap_size, &heap_src, &heap_target))
          {
            grub_free (memory_map);
            return grub_errno;
          }
        map_target = heap_src;
        grub_memcpy (map_target, memory_map, mmap_size);
        (void) heap_target;
      }
      bp->common.efi_mmap = (grub_xnu_ptr_t) (grub_addr_t) map_target;
      bp->common.efi_mmap_size = (grub_uint32_t) mmap_size;
      bp->common.efi_mem_desc_size = (grub_uint32_t) desc_size;
      bp->common.efi_mem_desc_version = desc_version;
      grub_free (memory_map);
    }

  /* The device tree the portable half built (xnu_devprop) is the
   * x86-shaped device-properties blob, not a flattened device tree.
   * On aarch64 the platform's own DTB is the machine description and
   * it arrives through firmware's config table, not through this
   * loader; leaving devtree at zero says "none built here" honestly
   * rather than passing an incompatible blob. */
  bp->common.devtree = devtree_target;
  bp->common.devtreelen = (grub_uint32_t) devtreelen;

  bp->common.heap_start = (grub_xnu_ptr_t) grub_xnu_heap_target_start;
  bp->common.heap_size = (grub_uint32_t) grub_xnu_heap_size;

  bp->ram_size = 0; /* filled by the kernel from ACPI/FDT, not inferred here */
  bp->fsbfreq = 0;

  stack_top = grub_xnu_heap_target_start + grub_xnu_heap_size + GRUB_XNU_PAGESIZE;

  return xnu_leave_firmware_and_enter (bootparams_target, stack_top);
}

void
grub_cpu_xnu_unload (void)
{
  /* Nothing arch-specific to release: the boot-parameter block and the
   * memory-map copy live in the loader heap the portable half owns. */
}

/* ---- devprop: present for link, honest about not being implemented ---- */

struct grub_xnu_devprop_device_descriptor
{
  int unused;
};

struct grub_xnu_devprop_device_descriptor *
grub_xnu_devprop_add_device (struct grub_efi_device_path *path, int length)
{
  (void) path;
  (void) length;
  grub_error (GRUB_ERR_NOT_IMPLEMENTED_YET,
              "device-properties are not implemented for aarch64");
  return NULL;
}

grub_err_t
grub_xnu_devprop_remove_device (struct grub_xnu_devprop_device_descriptor *dev)
{
  (void) dev;
  return GRUB_ERR_NONE;
}

grub_err_t
grub_xnu_devprop_remove_property (struct grub_xnu_devprop_device_descriptor *dev,
				  const char *name)
{
  (void) dev;
  (void) name;
  return GRUB_ERR_NONE;
}

grub_err_t
grub_xnu_devprop_add_property_utf8 (struct grub_xnu_devprop_device_descriptor *dev,
				    const char *name,
				    const void *data, grub_size_t datalen)
{
  (void) dev;
  (void) name;
  (void) data;
  (void) datalen;
  return GRUB_ERR_NONE;
}

grub_err_t
grub_xnu_devprop_add_property_utf16 (struct grub_xnu_devprop_device_descriptor *dev,
				     const grub_uint16_t *name,
				     const void *data, grub_size_t datalen)
{
  (void) dev;
  (void) name;
  (void) data;
  (void) datalen;
  return GRUB_ERR_NONE;
}

grub_err_t
grub_xnu_devprop_remove_property_utf8 (struct grub_xnu_devprop_device_descriptor *dev,
				       const char *name)
{
  (void) dev;
  (void) name;
  return GRUB_ERR_NONE;
}

void
grub_cpu_xnu_init (void)
{
  /* No arch-specific commands. In particular there is no
   * xnu_devprop_load: that command exists to ingest a firmware
   * device-properties dump, which is an Apple-firmware concept with no
   * aarch64 equivalent. */
}

/* ---- the two symbols the xnu module has to carry itself ----
 *
 * Both exist because this module is not linked into GRUB's own build:
 * it is a foreign .mod dropped next to stock ones, so it links against
 * whatever the shipping kernel chooses to export rather than against a
 * kernel built to match it. Neither symbol is in that export set, and
 * GRUB resolves a module's undefined symbols from the kernel plus
 * already-loaded modules -- so the module must define them.
 *
 * grub_memcpy. loader/xnu.c uses it heavily. Fedora/Debian GRUB kernels
 * export `memcpy` (the compiler-rt name) rather than GRUB's `grub_`
 * prefixed spelling, so the prefixed name resolves on a self-built x86
 * GRUB and not on these. Defining it here is legal C and ld -r then
 * leaves it defined instead of undefined, which is what GRUB needs: a
 * symbol is looked up against the module's own symtab first.
 *
 * grub_xnu_stack. On x86 loader/i386/xnu.c owns it, because that arch
 * half hands the kernel an esp the relocator has to place. Here the
 * stack top is computed inside grub_xnu_boot from the heap the portable
 * half already allocated, so nothing else reads this -- but
 * loader/xnu_resume.c references it, so the module needs the symbol.
 *
 * Neither is a behaviour change: one is a memcpy the compiler would
 * have emitted anyway, the other is state this port does not use. */

void *
grub_memcpy (void *dest, const void *src, grub_size_t n)
{
  grub_uint8_t *d = dest;
  const grub_uint8_t *s = src;

  while (n--)
    *d++ = *s++;

  return dest;
}

grub_xnu_ptr_t grub_xnu_stack;

/* Resume after hibernation: xnu_resume read the image header and left a
 * wake entry point and a stack in grub_xnu_entry_point and
 * grub_xnu_stack. The i386 half reloads relocator32 with eax/eip/esp;
 * the same state goes into relocator64's x0/pc/sp. Field for field, this
 * is loader/i386/xnu.c:800-811 with the arch's register names. */
grub_err_t
grub_xnu_boot_resume (void)
{
  struct grub_relocator64_state state = {0};

  state.x0 = (grub_uint64_t) grub_xnu_arg1;
  state.pc = (grub_uint64_t) grub_xnu_entry_point;
  state.sp = (grub_uint64_t) grub_xnu_stack;

  return grub_relocator64_boot (grub_xnu_relocator, state, 0);
}

void
grub_cpu_xnu_fini (void)
{
}
