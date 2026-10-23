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

/* aarch64 half of GRUB's relocator contract.
 *
 * The i386 version declares grub_relocator16/32/64_state and the
 * matching *_boot entry points, each carrying the register state the
 * target architecture is entered with. aarch64 upstream has neither.
 * This header follows the same shape, with the register set aarch64
 * actually has: x0-x3 (the ARM64 boot protocol's argument registers,
 * x0 being the device tree or, for the xnu loader, the boot-parameter
 * block) plus sp and pc.
 *
 * grub_relocator64_boot is the faithful counterpart of i386's
 * grub_relocator32_boot: it takes the relocator, the register state,
 * and the avoid-EFI-boot-services flag, and does not return on success.
 * That is what lets loader/arm64/xnu.c read like loader/i386/xnu.c --
 * build the parameters, fill in a state, call the boot entry. */

#ifndef GRUB_RELOCATOR_CPU_HEADER
#define GRUB_RELOCATOR_CPU_HEADER 1

#include <grub/types.h>
#include <grub/err.h>
#include <grub/relocator.h>

struct grub_relocator64_state
{
  grub_uint64_t x0;
  grub_uint64_t x1;
  grub_uint64_t x2;
  grub_uint64_t x3;
  grub_uint64_t sp;
  grub_uint64_t pc;
  /* [clean_start, clean_end): the loaded image's extent, cleaned to the
   * point of coherency before the MMU and data cache are turned off.
   * Filled in by grub_relocator64_boot, not by its callers. */
  grub_uint64_t clean_start;
  grub_uint64_t clean_end;
};

/* Enters the state described, never returning on success. When
 * avoid_efi_boot_services is zero, Boot Services are exited first --
 * matching i386's own flag semantics, where the caller that has already
 * left them passes 1. */
grub_err_t grub_relocator64_boot (struct grub_relocator *rel,
				  struct grub_relocator64_state state,
				  int avoid_efi_boot_services);

/* The final register load and branch, in assembly because no compiler
 * emits "set sp then branch to an arbitrary pc with these arguments"
 * reliably. Not called directly. */
void grub_relocator64_boot_asm (const struct grub_relocator64_state *state)
  __attribute__ ((noreturn));

#endif /* GRUB_RELOCATOR_CPU_HEADER */
