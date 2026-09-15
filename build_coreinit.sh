#!/bin/sh
set -e

ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD="$ROOT/build"
mkdir -p "$BUILD"

export PATH="/usr/lib/llvm-18/bin:$PATH"

CC=clang-18
TARGET=x86_64-apple-darwin
CFLAGS="-target $TARGET -std=c11 -ffreestanding -fno-stack-protector -mno-red-zone -mno-sse -mno-mmx -Wall -Wextra -I$ROOT/coreinit -I$ROOT/kernel -I$ROOT/include -O1 -g"

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

# -pagezero_size 0x1800000 (24 MiB, matching include/smp_layout.h's own
# BSP_COREINIT_LOAD_BASE): coreinit declares its OWN preferred load
# address here, via its own build, rather than the kernel deciding one
# for it at runtime. This is a deliberate principle, not an
# implementation detail -- the kernel should never itself choose where
# to place something; it should only ever act on a location it was
# actually given (see kernel.c's own kernel_boot, which now calls
# macho_load with target_base=0 for coreinit specifically because of
# this: 0 means "wherever your own vmaddr already says", not "no
# relocation" -- coreinit's vmaddr now IS 24 MiB, by this build's own
# declaration, so target_base=0 correctly lands it there without the
# kernel ever consulting BSP_COREINIT_LOAD_BASE itself). An AP's own
# copy is a genuinely different case, still legitimately relocated by
# a target_base the kernel receives from its actual caller (coreinit's
# own smp_bring_up_all, via the AP trampoline's region_base parameter)
# -- externally supplied, not self-decided, which is exactly the
# distinction this principle is about.
ld64.lld -arch x86_64 -platform_version macos 10.12 10.12 \
    -e _coreinit_main -pagezero_size 0x1800000 \
    -o "$BUILD/coreinit.macho" \
    "$BUILD/coreinit_main.o" "$BUILD/coreinit_serial.o" "$BUILD/coreinit_pci.o" \
    "$BUILD/coreinit_ata.o" "$BUILD/coreinit_irq.o" "$BUILD/coreinit_acpi.o" \
    "$BUILD/coreinit_smp.o" \
    "$BUILD/coreinit_mutex_mgr.o" "$BUILD/coreinit_mem_mgr.o" \
    "$BUILD/coreinit_lapic_timer.o" "$BUILD/coreinit_scheduler.o" \
    "$BUILD/coreinit_cluster_scheduler.o" "$BUILD/coreinit_proxy.o" \
    "$BUILD/coreinit_pagefault.o" "$BUILD/coreinit_job_memory.o" \
    "$BUILD/coreinit_job_exec.o" "$BUILD/coreinit_job_exec_switch.o" \
    "$BUILD/coreinit_cross_core_signal.o" "$BUILD/coreinit_arch_mmu.o"

echo "built $BUILD/coreinit.macho"
file "$BUILD/coreinit.macho"
