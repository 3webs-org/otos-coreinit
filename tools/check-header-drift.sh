#!/bin/sh
# check-header-drift.sh: detect drift between duplicated headers in otos-coreinit and saski.
#
# Usage:
#   tools/check-header-drift.sh
#
# Compares include/ headers that are duplicated across both repos:
#   - init_abi.h, smp_layout.h, ownership.h, memory_tokens.h, fdt.h
#   - lib/fdt.c (a verbatim source copy, not a header, duplicated for the
#     same reason and carrying the same silent-divergence risk)
#
# Also checks that contract headers coreinit consumes from saski exist:
#   - kernel/cap.h, kernel/schema.h, kernel/capnp.h
#
# Exit codes:
#   0 = all headers match (or SASKI_ROOT absent, for standalone CI)
#   1 = drift detected or contract headers missing

set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SASKI_ROOT=${SASKI_ROOT:-$ROOT/../saski}

# If SASKI_ROOT doesn't exist, warn and exit 0 (standalone checkout is OK)
if [ ! -d "$SASKI_ROOT" ]; then
    echo "WARNING: SASKI_ROOT not found at $SASKI_ROOT (standalone checkout?)" >&2
    echo "Skipping header drift check." >&2
    exit 0
fi

echo "Checking header drift between otos-coreinit and saski..."
echo "SASKI_ROOT=$SASKI_ROOT"
echo ""

DRIFT_DETECTED=0

# Duplicated include/ headers
for HEADER in init_abi.h smp_layout.h ownership.h memory_tokens.h fdt.h; do
    COREINIT_FILE="$ROOT/include/$HEADER"
    SASKI_FILE="$SASKI_ROOT/include/$HEADER"
    
    if [ ! -f "$COREINIT_FILE" ]; then
        echo "$HEADER: MISSING in otos-coreinit"
        DRIFT_DETECTED=1
        continue
    fi
    
    if [ ! -f "$SASKI_FILE" ]; then
        echo "$HEADER: MISSING in saski"
        DRIFT_DETECTED=1
        continue
    fi
    
    if cmp -s "$COREINIT_FILE" "$SASKI_FILE"; then
        echo "$HEADER: SAME"
    else
        echo "$HEADER: DIFFER"
        DRIFT_DETECTED=1
    fi
done

echo ""
echo "Checking contract headers (saski/kernel/ -> coreinit)..."

# Contract headers coreinit consumes from saski
for HEADER in cap.h schema.h capnp.h; do
    SASKI_FILE="$SASKI_ROOT/kernel/$HEADER"
    
    if [ ! -f "$SASKI_FILE" ]; then
        echo "kernel/$HEADER: MISSING"
        DRIFT_DETECTED=1
    elif [ ! -r "$SASKI_FILE" ]; then
        echo "kernel/$HEADER: NOT READABLE"
        DRIFT_DETECTED=1
    else
        echo "kernel/$HEADER: OK"
    fi
done

echo ""
echo "Checking duplicated SOURCE files (not only headers)..."

# lib/fdt.c is copied verbatim from saski, because libfdt gets compiled into
# whichever image needs it. A copy is still a copy: if saski fixes a DTB
# parsing bug, this repository silently keeps the old behaviour. It is covered
# here for the same reason the headers are -- a duplicated artifact with no
# equality assertion between the two repos is a divergence waiting to happen.
for SRC in lib/fdt.c; do
    COREINIT_FILE="$ROOT/$SRC"
    SASKI_FILE="$SASKI_ROOT/$SRC"

    if [ ! -f "$COREINIT_FILE" ]; then
        echo "$SRC: not present here (skipped)"
        continue
    fi
    if [ ! -f "$SASKI_FILE" ]; then
        echo "$SRC: MISSING in saski"
        DRIFT_DETECTED=1
        continue
    fi
    if cmp -s "$COREINIT_FILE" "$SASKI_FILE"; then
        echo "$SRC: SAME"
    else
        echo "$SRC: DIFFER"
        DRIFT_DETECTED=1
    fi
done

echo ""
if [ $DRIFT_DETECTED -eq 0 ]; then
    echo "All headers match and contract headers are present."
    exit 0
else
    echo "DRIFT DETECTED or headers missing."
    exit 1
fi
