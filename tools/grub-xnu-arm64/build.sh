#!/bin/sh
# Build GRUB's xnu loader for arm64-efi as a loadable .mod, against a
# stock distro GRUB. Same module, same grub.cfg as the i386 one.
#
# WHY A SEPARATE BUILD. Upstream gates grub-core/loader/xnu.c to x86
# (Makefile.core.def: enable = i386_efi, x86_64_efi) purely because the
# arch half only existed for x86 -- the portable half uses
# grub_xnu_ptr_t zero times. This builds that same portable file plus an
# arm64 arch half into one module, so a distro GRUB can load it.
#
# CC MUST BE AARCH64 GCC, NOT CLANG. GRUB's arm64 relocator
# (kern/arm64/dl.c) implements exactly these types: ABS64,
# ADD_ABS_LO12_NC, LDST64_ABS_LO12_NC, CALL26, JUMP26, PREL32,
# ADR_GOT_PAGE, LD64_GOT_LO12_NC, ADR_PREL_PG_HI21. clang fuses
# `adrp+add` into `R_AARCH64_LDST32_ABS_LO12_NC` / `LDST8` when it loads
# a narrow global, which the relocator rejects with "relocation is not
# implemented yet" -- and grub_dl_load failure is silent at the insmod
# prompt, so the module just does not appear. gcc emits only supported
# types.
#
# Uses: GRUB_SRC (configured source tree), GRUB_DEPS (dir with
# grub-efi-arm64-bin), AARCH64_GCC, LD_LLD, OBJCOPY.
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
GRUB_SRC=${GRUB_SRC:?set GRUB_SRC to a GRUB source tree}
GRUB_DEPS=${GRUB_DEPS:?set GRUB_DEPS to the dir holding grub-efi-arm64-bin}
AARCH64_GCC=${AARCH64_GCC:?set AARCH64_GCC to an aarch64 cross gcc}
LD_LLD=${LD_LLD:-ld.lld}
OBJCOPY=${OBJCOPY:-objcopy}
OUT=${OUT:-$HERE/build}

mkdir -p "$OUT/shim/grub/cpu"
# The configured tree's arm64 headers, plus this port's additions. GRUB
# upstream ships no include/grub/arm64/{memory,relocator,reloc,macho}.h --
# those are arch halves that exist only for the arches the xnu loader was
# gated to -- so the copies in include/grub/arm64/ cover the tree.
cp -r "$GRUB_SRC/include/grub/arm64/." "$OUT/shim/grub/cpu/" 2>/dev/null || true
cp "$HERE"/include/grub/arm64/*.h "$OUT/shim/grub/cpu/"

CFLAGS="-ffreestanding -nostdlib -Os -fno-builtin -fno-stack-protector
        -mno-outline-atomics -fshort-wchar
        -DGRUB_MACHINE_EFI=1 -D__aarch64__=1
        -I$OUT/shim -I$HERE -I$GRUB_SRC/include -I$GRUB_SRC -I$OUT"

cc1() { # <source> <grub-file-name> <output>
  # shellcheck disable=SC2086
  $AARCH64_GCC $CFLAGS -DGRUB_FILE="\"$2\"" -c "$1" -o "$OUT/$3.o"
}

cc1 "$GRUB_SRC/grub-core/loader/xnu.c"            loader/xnu.c            portable
cc1 "$HERE/loader/arm64/xnu.c"                    loader/arm64/xnu.c      arch
cc1 "$GRUB_SRC/grub-core/loader/xnu_resume.c"     loader/xnu_resume.c     resume
cc1 "$HERE/lib/arm64/relocator.c"                 lib/arm64/relocator.c   relocator
cc1 "$HERE/lib/arm64/relocator_boot.S"            lib/arm64/relocator_boot.S boot

$LD_LLD -r -o "$OUT/xnu.linked.o" \
  "$OUT/portable.o" "$OUT/arch.o" "$OUT/resume.o" \
  "$OUT/relocator.o" "$OUT/boot.o"

# .moddeps lists the stock modules that provide our undefined symbols, so
# `insmod xnu` pulls them the same way a distro module would.
printf 'xnu\0' > "$OUT/modname.bin"
printf 'macho\0extcmd\0bitmap\0boot\0' > "$OUT/moddeps.bin"
$OBJCOPY --add-section .modname="$OUT/modname.bin" \
         --add-section .moddeps="$OUT/moddeps.bin" \
         "$OUT/xnu.linked.o" "$OUT/xnu.mod"

# Fail the build, not the boot, if the relocator cannot handle what we
# emitted. A silent load failure in GRUB looks identical to a module that
# simply has no commands.
# The nine types grub-core/kern/arm64/dl.c implements, verbatim from its
# switch. A module carrying any other type is rejected at load time with
# "relocation 0x.. is not implemented yet", and grub_dl_load reports that
# failure so quietly that `insmod` looks successful and the commands are
# simply absent. Catch it here instead.
SUPPORTED="R_AARCH64_ABS64
R_AARCH64_ADD_ABS_LO12_NC
R_AARCH64_LDST64_ABS_LO12_NC
R_AARCH64_CALL26
R_AARCH64_JUMP26
R_AARCH64_PREL32
R_AARCH64_ADR_GOT_PAGE
R_AARCH64_LD64_GOT_LO12_NC
R_AARCH64_ADR_PREL_PG_HI21"

printf '%s\n' "$SUPPORTED" | sort -u > "$OUT/supported-relocs.txt"
${READELF:-readelf} -rW "$OUT/xnu.mod" | awk '{print $3}' | grep '^R_' \
  | sort -u > "$OUT/mine-relocs.txt"
BAD=$(comm -23 "$OUT/mine-relocs.txt" "$OUT/supported-relocs.txt")
if [ -n "$BAD" ]; then
  echo "unsupported relocation types (kern/arm64/dl.c cannot apply these):" >&2
  echo "$BAD" >&2
  exit 1
fi

echo "built $OUT/xnu.mod ($(wc -c < "$OUT/xnu.mod") bytes)"
