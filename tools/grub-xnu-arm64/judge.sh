#!/bin/sh
# QEMU judge for the arm64 xnu loader: boot a stock Debian GRUB on QEMU
# virt with the x86 judge's grub.cfg, unchanged, and require the test
# kernel to report every item of the entry contract.
#
# Uses everything build.sh uses, plus: CC (clang), QEMU (qemu-system-
# aarch64), MTOOLS (dir with mcopy/mdel), BASE_IMG (a FAT disk image with
# Debian's arm64 GRUB installed under /EFI/debian -- the saski pin's
# build/arm-judge/disk.img), AAVMF (dir with AAVMF_CODE.no-secboot.fd and
# AAVMF_VARS.fd).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-$HERE/build}
: "${CC:?}" "${QEMU:?}" "${MTOOLS:?}" "${BASE_IMG:?}" "${AAVMF:?}"

OUT=$OUT sh "$HERE/build.sh"

T=$OUT/judge; mkdir -p "$T"
"$CC" -target aarch64-none-elf -c "$HERE/test-kernel/entry.S" -o "$T/entry.o"
"$OBJCOPY" -O binary "$T/entry.o" "$T/entry.bin"
python3 "$HERE/test-kernel/mkmacho.py" "$T/entry.bin" "$T/saski.macho"

# Byte-for-byte the x86 judge's real-boot grub.cfg (saski tools/qemu-boot.sh).
cat > "$T/grub.cfg" <<'CFG'
set prefix=(hd0)/EFI/debian
set root=(hd0)
insmod xnu
echo "[grubcfg] root is hd0 ; loading saski kernel: /saski.macho"
xnu_kernel64 /saski.macho
echo "[grubcfg] kernel loaded, booting"
boot
CFG

cp "$BASE_IMG" "$T/disk.img"
"$MTOOLS/mcopy" -o -i "$T/disk.img" "$OUT/xnu.mod" ::/EFI/debian/arm64-efi/xnu.mod
"$MTOOLS/mcopy" -o -i "$T/disk.img" "$T/saski.macho" ::/saski.macho
"$MTOOLS/mcopy" -o -i "$T/disk.img" "$T/grub.cfg" ::/EFI/debian/grub.cfg
cp "$AAVMF/AAVMF_VARS.fd" "$T/vars.fd"

"$QEMU" -machine virt -cpu cortex-a57 -m 512 -smp 1 \
  -drive if=pflash,format=raw,readonly=on,file="$AAVMF/AAVMF_CODE.no-secboot.fd" \
  -drive if=pflash,format=raw,file="$T/vars.fd" \
  -drive file="$T/disk.img",format=raw,if=virtio \
  -display none -serial file:"$T/serial.log" -monitor none -no-reboot &
QP=$!
i=0
while [ $i -lt ${TIMEOUT:-60} ] && ! grep -q '\[xnu-arm64-test\] done' "$T/serial.log" 2>/dev/null; do
  sleep 1; i=$((i + 1))
done
kill $QP 2>/dev/null || true; wait $QP 2>/dev/null || true

fail=0
for want in '\[grubcfg\] kernel loaded, booting' \
            '\[xnu-arm64-test\] entered' \
            '\[xnu-arm64-test\] x0 = boot params v2' \
            '\[xnu-arm64-test\] efi_system_table valid' \
            '\[xnu-arm64-test\] mmu off' \
            '\[xnu-arm64-test\] sp set' \
            '\[xnu-arm64-test\] done'; do
  if grep -q "$want" "$T/serial.log"; then echo "PASS $want"; else echo "FAIL $want"; fail=1; fi
done
[ $fail = 0 ] && echo "JUDGE PASS" || { echo "JUDGE FAIL (log: $T/serial.log)"; exit 1; }
