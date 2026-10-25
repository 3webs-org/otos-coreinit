#!/bin/sh
# dev-env.sh: resolve LLVM 18 toolchain from Nix and run a command with it.
#
# Usage:
#   tools/dev-env.sh [command...]
#
# Default command: configure (if needed) and build coreinit with CMake for
# ARCH (x86_64 or aarch64, default x86_64). Switching ARCH on an existing
# build/ directory isn't supported by CMake -- rm -rf build first.
#
# Sets LLVM_BIN, CC, LD64 to point at nix-resolved ld64.lld and Apple clang.
# Nix store paths are resolved at runtime (not hardcoded) so this works
# across nixpkgs updates.
#
# Apple clang CAN emit x86_64-apple-darwin objects; we only need Nix for
# ld64.lld (the linker), since Xcode's ld requires libSystem for dynamic
# executables and we're building a freestanding Mach-O.
#
# Example:
#   tools/dev-env.sh                    # builds coreinit
#   ARCH=aarch64 tools/dev-env.sh        # builds coreinit for aarch64
#   tools/dev-env.sh sh -c 'echo $LLVM_BIN'

set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ARCH=${ARCH:-x86_64}

# Resolve lld-18 from Nix (ld64.lld is the only tool we need from Nix)
echo "Resolving ld64.lld from Nix..." >&2
LLD_STORE=$(/nix/var/nix/profiles/default/bin/nix build --no-link --print-out-paths nixpkgs#lld_18)

export LLVM_BIN="$LLD_STORE/bin"
export CC="clang"
export LD64="$LLD_STORE/bin/ld64.lld"

echo "LLVM_BIN=$LLVM_BIN" >&2
echo "CC=$CC (Apple clang from Xcode)" >&2
echo "LD64=$LD64" >&2

# Verify ld64.lld is actually present and runnable
if ! "$LD64" --version >/dev/null 2>&1; then
    echo "ERROR: ld64.lld not functional at $LD64" >&2
    exit 1
fi

# Default command: configure + build with CMake
if [ $# -eq 0 ]; then
    cmake -S "$ROOT" -B "$ROOT/build" \
        -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/toolchain-$ARCH.cmake" \
        -DCMAKE_C_COMPILER="$CC" -DLD64="$LD64"
    set -- cmake --build "$ROOT/build"
fi

exec "$@"
