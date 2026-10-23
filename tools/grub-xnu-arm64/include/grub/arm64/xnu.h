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

/* aarch64 half of the xnu loader's arch contract.
 *
 * This mirrors include/grub/i386/xnu.h field for field, with one
 * substantive change: every pointer in the boot-parameter structure is
 * 64-bit. The x86 header hardcodes grub_xnu_ptr_t as uint32_t because
 * the XNU boot protocol it mirrors was written for 32-bit-capable
 * x86_64 Macs entering through a 32-bit shim; on aarch64 there is no
 * such shim and no 32-bit mode to enter through, so the natural width
 * applies.
 *
 * Everything ABOVE this header -- grub-core/loader/xnu.c, the portable
 * 1556 lines that build the device tree, resolve kexts, write
 * device-properties and drive grub_loader -- uses only these names and
 * GRUB_XNU_PAGESIZE. It contains no use of grub_xnu_ptr_t at all, which
 * is why the same portable file can serve both architectures once each
 * provides its own version of this header.
 *
 * NOTE ON THE ENTRY CONVENTION. On x86_64 the kernel is entered through
 * grub_relocator32_boot with a 32-bit state, because XNU's own entry
 * expects a legacy-mode handoff. On aarch64 no relocator exists in GRUB
 * (grub-core/lib/arm64/ contains only setjmp.S), and none is needed for
 * the shape this header serves: the arch half enters the Mach-O image
 * directly, with the boot-parameter block in x0. That is the ARM
 * analogue of the x86 convention -- one register carrying one pointer
 * to one structure -- not a departure from it. */

#ifndef GRUB_CPU_XNU_H
#define GRUB_CPU_XNU_H 1

#include <grub/err.h>
#include <grub/efi/api.h>
#include <grub/types.h>
/* The portable loader gets the relocator API transitively on x86 via
 * <grub/cpu/relocator.h>. aarch64 has no per-cpu relocator header, so
 * the neutral one is included here -- which is what the portable loader
 * actually uses (grub_relocator_new / alloc_chunk_addr / unload), not
 * any arch-specific entry state. */
#include <grub/relocator.h>
#include <grub/cpu/relocator.h>

#define GRUB_XNU_PAGESIZE 4096
typedef grub_uint64_t grub_xnu_ptr_t;

struct grub_xnu_boot_params_common
{
  /* Command line passed to xnu. */
  grub_uint8_t cmdline[1024];

  /* Later are the same as EFI's get_memory_map (). */
  grub_xnu_ptr_t efi_mmap;
  grub_uint32_t efi_mmap_size;
  grub_uint32_t efi_mem_desc_size;
  grub_uint32_t efi_mem_desc_version;

  /* Later are video parameters. */
  grub_xnu_ptr_t lfb_base;
#define GRUB_XNU_VIDEO_SPLASH 1
#define GRUB_XNU_VIDEO_TEXT_IN_VIDEO 2
  grub_uint32_t lfb_mode;
  grub_uint32_t lfb_line_len;
  grub_uint32_t lfb_width;
  grub_uint32_t lfb_height;
  grub_uint32_t lfb_depth;

  /* Pointer to device tree and its len. */
  grub_xnu_ptr_t devtree;
  grub_uint32_t devtreelen;

  /* First used address by kernel or boot structures. */
  grub_xnu_ptr_t heap_start;
  /* Last used address by kernel or boot structures minus previous value. */
  grub_uint32_t heap_size;
  /* First memory page containing runtime code or data. */
  grub_xnu_ptr_t efi_runtime_first_page;
  /* Number of runtime pages. */
  grub_uint32_t efi_runtime_npages;
};

struct grub_xnu_boot_params_v1
{
  struct grub_xnu_boot_params_common common;
};

#define GRUB_XNU_BOOTARGSV1_VERMINOR 5
#define GRUB_XNU_BOOTARGSV1_VERMAJOR 1

struct grub_xnu_boot_params_v2
{
  /* Version of the struct -- if this is not what the kernel expects it
   * will attempt to use the v1 layout instead. */
  grub_uint16_t verminor;
  grub_uint16_t vermajor;

  grub_uint8_t efi_uintnbits;

  grub_uint8_t unused0[3];

  struct grub_xnu_boot_params_common common;

  grub_xnu_ptr_t efi_runtime_first_page_virtual;

  /* Physical address of the EFI system table. */
  grub_xnu_ptr_t efi_system_table;

  grub_uint32_t unused1[9];

  /* Total RAM as discovered from the EFI memory map. */
  grub_uint64_t ram_size;

  /* CPU frequency. On aarch64 this comes from the generic timer's
   * CNTFRQ_EL0 rather than from a calibrated TSC; the field is kept at
   * the same offset so a consumer reading one layout works on both. */
  grub_uint64_t fsbfreq;

  grub_uint32_t unused2[734];
};

#define GRUB_XNU_BOOTARGSV2_VERMINOR 0
#define GRUB_XNU_BOOTARGSV2_VERMAJOR 2

/* Device-properties: the aarch64 port carries the structure definitions
 * so the portable loader compiles unchanged, but the x86 command that
 * populates them from a firmware dump (xnu_devprop_load) has no aarch64
 * equivalent in this port -- see the arch half's own note. */
struct grub_xnu_devprop_header
{
  grub_uint32_t length;
  grub_uint32_t num_devices;
};

struct grub_xnu_devprop_device_header
{
  grub_uint32_t length;
  grub_uint32_t num_properties;
};

void grub_cpu_xnu_unload (void);

struct grub_xnu_devprop_device_descriptor;

struct grub_xnu_devprop_device_descriptor *
grub_xnu_devprop_add_device (struct grub_efi_device_path *path, int length);
grub_err_t
grub_xnu_devprop_remove_device (struct grub_xnu_devprop_device_descriptor *dev);
grub_err_t
grub_xnu_devprop_remove_property (struct grub_xnu_devprop_device_descriptor *dev,
				  const char *name);
grub_err_t
grub_xnu_devprop_add_property_utf8 (struct grub_xnu_devprop_device_descriptor *dev,
				    const char *name,
				    const void *data, grub_size_t datalen);
grub_err_t
grub_xnu_devprop_add_property_utf16 (struct grub_xnu_devprop_device_descriptor *dev,
				     const grub_uint16_t *name,
				     const void *data, grub_size_t datalen);
grub_err_t
grub_xnu_devprop_remove_property_utf8 (struct grub_xnu_devprop_device_descriptor *dev,
				       const char *name);
void grub_cpu_xnu_init (void);
void grub_cpu_xnu_fini (void);

extern grub_xnu_ptr_t grub_xnu_entry_point;
extern grub_xnu_ptr_t grub_xnu_arg1;
extern char grub_xnu_cmdline[1024];

/* loader/xnu_resume.c writes this on every arch; on i386 it is a
 * grub_uint32_t because the relocator32 state it feeds is 32-bit. Here
 * the stack is an address the arch half hands to relocator64, so it is
 * pointer-sized. The name and the role are the same as x86's; only the
 * width differs, which is the whole point of an arch half. */
extern grub_xnu_ptr_t grub_xnu_stack;

/* loader/xnu.c uses grub_memcpy throughout. Stock GRUB kernels export
 * the compiler-rt spelling `memcpy` instead, so the module carries its
 * own -- see the note beside the definition in loader/arm64/xnu.c. */
void *grub_memcpy (void *dest, const void *src, grub_size_t n);
grub_err_t grub_xnu_boot (void);

#endif /* GRUB_CPU_XNU_H */
