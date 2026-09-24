#!/bin/sh
set -e

ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD="$ROOT/build"
mkdir -p "$BUILD"

# coreinit's own sources include two headers that belong to the kernel it
# runs under rather than to coreinit itself (cap.h and schema.h -- the
# capability ABI they share). That kernel lives in its own repo, so where
# it is can't be derived from $ROOT the way everything else here is; a
# sibling checkout is merely the default, and SASKI_ROOT overrides it.
SASKI_ROOT="${SASKI_ROOT:-$(cd "$ROOT/../saski" 2>/dev/null && pwd)}"
if [ ! -d "$SASKI_ROOT/kernel" ]; then
    echo "build_coreinit.sh: no kernel/ at '${SASKI_ROOT:-<unset>}' -- set SASKI_ROOT to the saski checkout" >&2
    exit 1
fi

# include/ here is a byte-for-byte copy of ../saski/include/. That is a
# deliberate arrangement -- coreinit builds against a specific ABI rather
# than whatever the kernel tree happens to say today -- but a copy that
# drifts silently is worse than either having one or not. So: check.
#
# A real divergence should be an explicit act (update the copy, or decide
# the ABI genuinely forked), never something discovered later by debugging
# a struct that two binaries disagree about the shape of.
for h in fdt.h init_abi.h memory_tokens.h ownership.h smp_layout.h; do
    if [ -f "$SASKI_ROOT/include/$h" ] && \
       ! cmp -s "$ROOT/include/$h" "$SASKI_ROOT/include/$h"; then
        echo "build_coreinit.sh: include/$h differs from $SASKI_ROOT/include/$h" >&2
        echo "  These are the shared kernel/init ABI and must match." >&2
        echo "  diff -u '$ROOT/include/$h' '$SASKI_ROOT/include/$h'" >&2
        exit 1
    fi
done

export PATH="/usr/lib/llvm-18/bin:$PATH"

CC=clang-18
TARGET=x86_64-apple-darwin
# -fpie is explicit, not inherited: coreinit is relocated wholesale into an
# AP's own region (smp.c), and everything that finds its own image at runtime
# -- &__mh_execute_header above all -- is only correct if references are
# RIP-relative rather than absolute. ld64 already marks the output PIE; this
# makes codegen agree rather than relying on it happening to.
CFLAGS="-target $TARGET -std=c11 -ffreestanding -fno-stack-protector -fpie -mno-red-zone -mno-sse -mno-mmx -Wall -Wextra -I$ROOT/coreinit -I$SASKI_ROOT/kernel -I$ROOT/include -O1 -g"

"$CC" $CFLAGS -c "$ROOT/coreinit/coreinit.c" -o "$BUILD/coreinit_main.o"
"$CC" $CFLAGS -c "$ROOT/coreinit/serial.c" -o "$BUILD/coreinit_serial.o"
"$CC" $CFLAGS -c "$ROOT/coreinit/pci.c" -o "$BUILD/coreinit_pci.o"
"$CC" $CFLAGS -c "$ROOT/coreinit/ata.c" -o "$BUILD/coreinit_ata.o"
"$CC" $CFLAGS -c "$ROOT/coreinit/irq.c" -o "$BUILD/coreinit_irq.o"
"$CC" $CFLAGS -c "$ROOT/coreinit/acpi.c" -o "$BUILD/coreinit_acpi.o"
"$CC" $CFLAGS -c "$ROOT/coreinit/smp.c" -o "$BUILD/coreinit_smp.o"
"$CC" $CFLAGS -c "$ROOT/coreinit/mutex_mgr.c" -o "$BUILD/coreinit_mutex_mgr.o"
"$CC" $CFLAGS -c "$ROOT/coreinit/mem_mgr.c" -o "$BUILD/coreinit_mem_mgr.o"
"$CC" $CFLAGS -c "$ROOT/coreinit/lapic_timer.c" -o "$BUILD/coreinit_lapic_timer.o"
"$CC" $CFLAGS -c "$ROOT/coreinit/scheduler.c" -o "$BUILD/coreinit_scheduler.o"
"$CC" $CFLAGS -c "$ROOT/coreinit/cluster_scheduler.c" -o "$BUILD/coreinit_cluster_scheduler.o"
"$CC" $CFLAGS -c "$ROOT/coreinit/proxy.c" -o "$BUILD/coreinit_proxy.o"
"$CC" $CFLAGS -c "$ROOT/coreinit/pagefault.c" -o "$BUILD/coreinit_pagefault.o"
"$CC" $CFLAGS -c "$ROOT/coreinit/job_memory.c" -o "$BUILD/coreinit_job_memory.o"
"$CC" $CFLAGS -c "$ROOT/coreinit/job_exec.c" -o "$BUILD/coreinit_job_exec.o"
"$CC" -target $TARGET -c "$ROOT/coreinit/job_exec_switch.S" -o "$BUILD/coreinit_job_exec_switch.o"
"$CC" $CFLAGS -c "$ROOT/coreinit/cross_core_signal_x86_64.c" -o "$BUILD/coreinit_cross_core_signal.o"
"$CC" $CFLAGS -c "$ROOT/coreinit/arch_mmu_x86_64.c" -o "$BUILD/coreinit_arch_mmu.o"
"$CC" $CFLAGS -c "$ROOT/coreinit/arch_cpu_x86_64.c" -o "$BUILD/coreinit_arch_cpu.o"

# The kernel's own Mach-O parser, compiled into coreinit too rather than
# reimplemented here. coreinit needs macho_image_size to measure its OWN
# loaded image (mem_mgr.c), and will need macho_load itself once init is
# split into stages -- a later stage arrives as bytes in RAM that
# something has to load, and by then it is init's job, not the kernel's.
# It pulls in nothing kernel-specific: just macho.h and a kprintf, which
# resolves to coreinit's own serial.h because -I$ROOT/coreinit comes
# first.
"$CC" $CFLAGS -c "$SASKI_ROOT/kernel/macho.c" -o "$BUILD/coreinit_macho.o"

# -pagezero_size 0x1000 -- one page, purely a null-address trap. coreinit
# deliberately does NOT declare where it wants to live any more.
#
# It used to say 24 MiB here, and the kernel honoured that by loading it at
# its own vmaddr (macho_load with target_base=0). That number was never a
# design decision: it was empirical collision-avoidance from before the
# kernel could read a memory map -- originally 2 MiB, pushed up because OVMF
# put the ACPI RSDP somewhere a 2 MiB image overwrote. The kernel now reads
# GRUB's real EFI memory map and places init in memory actually known to be
# free, so a guess is no longer needed and would only constrain where init
# can go.
#
# Two things this also fixes. An AP's copy is loaded at
# region_base + CORE_INIT_OFFSET, and macho_load adds the image's own
# vmaddr -- so a 24 MiB vmaddr put every AP's init 24 MiB PAST its own
# 4 MiB region (observed at 0x5900000 for a region based at 0x4000000),
# which with enough cores lands inside another core's region. And a
# link-time address of 24 MiB is simply impossible on a part with less
# address space than that, which is exactly the small-target case.

ld64.lld -arch x86_64 -platform_version macos 10.12 10.12 \
    -e _coreinit_main -pagezero_size 0x1000 \
    -o "$BUILD/coreinit.macho" \
    "$BUILD/coreinit_main.o" "$BUILD/coreinit_serial.o" "$BUILD/coreinit_pci.o" \
    "$BUILD/coreinit_ata.o" "$BUILD/coreinit_irq.o" "$BUILD/coreinit_acpi.o" \
    "$BUILD/coreinit_smp.o" \
    "$BUILD/coreinit_mutex_mgr.o" "$BUILD/coreinit_mem_mgr.o" \
    "$BUILD/coreinit_lapic_timer.o" "$BUILD/coreinit_scheduler.o" \
    "$BUILD/coreinit_cluster_scheduler.o" "$BUILD/coreinit_proxy.o" \
    "$BUILD/coreinit_pagefault.o" "$BUILD/coreinit_job_memory.o" \
    "$BUILD/coreinit_job_exec.o" "$BUILD/coreinit_job_exec_switch.o" \
    "$BUILD/coreinit_cross_core_signal.o" "$BUILD/coreinit_arch_mmu.o" \
    "$BUILD/coreinit_arch_cpu.o" \
    "$BUILD/coreinit_macho.o"

echo "built $BUILD/coreinit.macho"
file "$BUILD/coreinit.macho"
