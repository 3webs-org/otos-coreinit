#!/bin/sh
# dev-env.sh: resolve LLVM 18 toolchain from Nix and run a command with it.
#
# Usage:
#   tools/dev-env.sh [command...]
#
# Default command: ./build_coreinit.sh
#
# Sets LLVM_BIN, CC, LD64 to point at nix-resolved ld64.lld and Apple clang,
# then execs the given command. Nix store paths are resolved at runtime
# (not hardcoded) so this works across nixpkgs updates.
#
# Apple clang CAN emit x86_64-apple-darwin objects; we only need Nix for
# ld64.lld (the linker), since Xcode's ld requires libSystem for dynamic
# executables and we're building a freestanding Mach-O.
#
# Example:
#   tools/dev-env.sh                    # builds coreinit
#   tools/dev-env.sh ./build_coreinit.sh
#   tools/dev-env.sh sh -c 'echo $LLVM_BIN'

set -e

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

# Default command: build_coreinit.sh
if [ $# -eq 0 ]; then
    set -- ./build_coreinit.sh
fi

exec "$@"
