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

/* aarch64 half of GRUB's Mach-O loader contract.
 *
 * The x86 version (include/grub/i386/macho.h) is four lines: it maps the
 * host CPU type onto GRUB_MACHO_CPUTYPE_IA32/AMD64 so machoXX.c can
 * decide whether a given slice is loadable on THIS machine. The arm64
 * version is the same idea with one honest difference: GRUB_MACHO_CPUTYPE_ARM64
 * does not exist upstream -- include/grub/macho.h only names IA32 and
 * AMD64 -- so this header defines the constant locally and a loader
 * built against it recognises arm64 slices.
 *
 * That local constant is the whole reason a stock arm64-efi GRUB ships
 * macho.mod but cannot boot a Mach-O kernel: the module is compiled
 * without any notion of an arm64 CPU type, so every slice fails the
 * host check. This header plus the patched macho.h is what makes the
 * module useful on this architecture.
 *
 * NOTE: GRUB_MACHO_CPUTYPE_ARM64 is 0x0100000C -- CPU_TYPE_ARM64, the
 * same value darwin's own <mach/machine.h> uses and the value our
 * kernel's Mach-O actually carries. */

#ifndef GRUB_CPU_MACHO_H
#define GRUB_CPU_MACHO_H 1

#include <grub/macho.h>

/* CPU_TYPE_ARM64, from <mach/machine.h>. Defined here because
 * include/grub/macho.h (shared) names only IA32 and AMD64. */
#ifndef GRUB_MACHO_CPUTYPE_ARM64
#define GRUB_MACHO_CPUTYPE_ARM64 0x0100000C
#endif

#define GRUB_MACHO_CPUTYPE_IS_HOST32(x) (0)
#define GRUB_MACHO_CPUTYPE_IS_HOST64(x) ((x) == GRUB_MACHO_CPUTYPE_ARM64)
#define GRUB_MACHO_CPUTYPE_IS_HOST_CURRENT(x) ((x) == GRUB_MACHO_CPUTYPE_ARM64)

#endif
