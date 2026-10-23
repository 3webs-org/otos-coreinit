#!/bin/sh
# Stage a GRUB source tree for the arm64 xnu loader build.
#
# THE VERSION MUST MATCH THE TARGET KERNEL. GRUB changed an ABI between
# 2.12 (what Debian ships, what this project boots) and 2.14: grub_error
# became a macro that passes __FILE__, __FUNCTION__ and __LINE__ ahead of
# the format string. Compiling the loader against 2.14 headers and
# loading it into a 2.12 kernel does not fail to link -- grub_error is a
# kernel export either way -- so every error message comes out as the
# source path and, worse, every error value a command returns is read
# from the wrong place. That presents as "you need to load the kernel
# first" after a command that in fact ran.
#
# Two source changes beyond the version pin:
#
# 1. loader/xnu.c hardcodes 0x0fffffff as the image-base mask. That is
#    the x86 convention (see the note on GRUB_XNU_IMAGE_BASE_MASK in
#    include/grub/arm64/xnu.h); an aarch64 kernel links far above it.
#    Replaced with a macro the arch header defines.
#
# 2. The 64-bit entry point is read through GRUB_XNU_GET_ENTRY_POINT64:
#    macho.mod assumes x86_THREAD_STATE64; arm64 carries ARM_THREAD_STATE64.
#
# 3. loader/xnu.c is only listed for the x86 modules in
#    Makefile.core.def, so nothing needs changing there -- this build
#    compiles the file directly.
#
# Usage: prepare-src.sh <source-tree>   (mutates the tree in place)
set -e

SRC=${1:?usage: prepare-src.sh <grub-source-tree>}
HERE=$(cd "$(dirname "$0")" && pwd)

XNU_MASK=0x0fffffff

if [ ! -f "$SRC/grub-core/loader/xnu.c" ]; then
  echo "prepare-src.sh: $SRC does not look like a GRUB source tree" >&2
  exit 1
fi

if grep -q "$XNU_MASK" "$SRC/grub-core/loader/xnu.c"; then
  sed -i.bak "s/$XNU_MASK/GRUB_XNU_IMAGE_BASE_MASK/g" \
    "$SRC/grub-core/loader/xnu.c"
  echo "patched image-base mask in loader/xnu.c"
else
  echo "image-base mask already patched"
fi

if grep -q 'grub_macho_get_entry_point64 (macho' "$SRC/grub-core/loader/xnu.c"; then
  sed -i.bak 's/grub_macho_get_entry_point64 (macho/GRUB_XNU_GET_ENTRY_POINT64 (macho/' \
    "$SRC/grub-core/loader/xnu.c"
  echo "patched entry-point reader in loader/xnu.c"
fi

# The mask macro has to be in scope for loader/xnu.c, which includes
# <grub/cpu/xnu.h>.
if ! grep -q GRUB_XNU_IMAGE_BASE_MASK "$HERE/include/grub/arm64/xnu.h"; then
  echo "prepare-src.sh: arch header lacks GRUB_XNU_IMAGE_BASE_MASK" >&2
  exit 1
fi
echo "source tree ready: $SRC"
