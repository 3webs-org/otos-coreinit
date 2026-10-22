#!/bin/sh
# Self-check harness for the ARM boot-path integration unit (not part of the build).
export PATH=/nix/var/nix/profiles/default/bin:$PATH
export CC=$(xcrun -f clang)
LD=$(nix build --no-link --print-out-paths nixpkgs#lld_18 2>/dev/null | head -1)
export LD64=$LD/bin/ld64.lld
export LLVM_BIN=$LD/bin
export SASKI_ROOT=$(sh tools/saski-pin.sh 2>/dev/null)

vmaddr() {
python3 -c "
import struct,sys
f=open('build/coreinit.macho','rb').read()
off=struct.unpack('<I',f[16:20])[0]
ncmds=struct.unpack('<I',f[12:16])[0]
p=off
for _ in range(ncmds):
    cmd,sz=struct.unpack('<II',f[p:p+8])
    if cmd==0x19:
        segname=f[p+8:p+24].rstrip(b'\x00').decode()
        vmaddr=struct.unpack('<Q',f[p+24:p+32])[0]
        if segname=='__TEXT': print(hex(vmaddr))
    p+=sz
"
}

case "$1" in
  arm)
    rm -rf build
    ARCH=aarch64 sh tools/dev-env.sh
    echo "AARCH64_EXIT=$?"
    vmaddr
    file build/coreinit.macho
    ;;
  x86)
    rm -rf build
    sh tools/dev-env.sh
    echo "X86_EXIT=$?"
    vmaddr
    ;;
  *) echo "usage: $0 arm|x86" >&2; exit 2;;
esac