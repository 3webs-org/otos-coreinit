#!/bin/sh
# Build and run the ACPI topology host test. Needs SASKI_ROOT (for cap.h).
set -e
HERE=$(cd "$(dirname "$0")" && pwd); R=$HERE/../..
: "${SASKI_ROOT:?}"; O=${TMPDIR:-/tmp}/topology-aarch64-test; mkdir -p "$O"
I="-I$R -I$R/coreinit -I$R/include -I$SASKI_ROOT/kernel -I$SASKI_ROOT/include"
xcrun clang -w $I -c "$R/coreinit/topology_aarch64.c" -o "$O/topo.o"
xcrun clang -w $I "$HERE/test.c" "$O/topo.o" -Wl,-undefined,dynamic_lookup -o "$O/t"
"$O/t" "$HERE/qemu-virt-acpi.bin" 2
"$O/t" "$HERE/qemu-virt-gic3-acpi.bin" 3
