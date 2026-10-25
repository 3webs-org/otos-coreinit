#!/bin/sh
# saski-pin.sh: materialise a pinned copy of saski at a specific revision.
#
# Usage:
#   tools/saski-pin.sh [rev]        # rev defaults to 'HEAD'
#
# Prints the path to the pinned copy on stdout (all other output to stderr).
# The pinned copy is extracted from git archive, so uncommitted changes in
# saski's working tree are deliberately excluded.
#
# Example:
#   cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-x86_64.cmake -DSASKI_ROOT=$(tools/saski-pin.sh)
#   cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-x86_64.cmake -DSASKI_ROOT=$(tools/saski-pin.sh v1.2.3)

set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REV="${1:-HEAD}"

# Resolve SASKI_ROOT (default: $ROOT/../saski)
SASKI_ROOT="${SASKI_ROOT:-$ROOT/../saski}"

if [ ! -d "$SASKI_ROOT/.git" ]; then
    echo "ERROR: SASKI_ROOT does not point to a git repository: $SASKI_ROOT" >&2
    echo "Set SASKI_ROOT to the saski repository root." >&2
    exit 1
fi

# Resolve the revision to a full commit sha
SHA=$(git -C "$SASKI_ROOT" rev-parse "$REV" 2>/dev/null) || {
    echo "ERROR: Cannot resolve revision '$REV' in $SASKI_ROOT" >&2
    exit 1
}

# Use first 12 characters of sha for the cache directory name
SHA12=$(echo "$SHA" | cut -c1-12)
CACHE_DIR="$ROOT/.saski-pin/$SHA12"

# Check if already materialized
if [ -d "$CACHE_DIR" ] && [ -n "$(ls -A "$CACHE_DIR" 2>/dev/null)" ]; then
    echo "Using cached saski at $SHA12" >&2
else
    echo "Materializing saski at $SHA12 from $REV..." >&2
    mkdir -p "$CACHE_DIR"
    
    # Extract the committed tree at this sha
    git -C "$SASKI_ROOT" archive "$SHA" | tar -x -C "$CACHE_DIR" || {
        echo "ERROR: git archive failed for revision $SHA" >&2
        rm -rf "$CACHE_DIR"
        exit 1
    }
    
    echo "Extracted saski $SHA12 to cache" >&2
fi

# Print the path (stdout only, for capture)
echo "$CACHE_DIR"
