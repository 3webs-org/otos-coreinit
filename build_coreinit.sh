#!/bin/sh
set -e

# ARCHITECTURE CONVENTION
# -----------------------
# Arch-specific implementation lives in <header-stem>_<arch>.c files
# (e.g., arch_mmu_x86_64.c, cross_core_signal_x86_64.c). Headers stay
# arch-neutral and define the seam (e.g., arch_mmu.h, cross_core_signal.h).
#
# Adding a new architecture means adding a file list below, not adding
# #ifdef ladders inside shared files. The ARCH variable selects which
# list to compile.

ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD="$ROOT/build"
mkdir -p "$BUILD"

# Architecture: x86_64 or aarch64
ARCH=${ARCH:-x86_64}

# Toolchain paths: overridable for macOS/Nix or other non-Debian environments
LLVM_BIN=${LLVM_BIN:-/usr/lib/llvm-18/bin}
export PATH="$LLVM_BIN:$PATH"

CC=${CC:-clang-18}
LD64=${LD64:-ld64.lld}

# Contract headers from the kernel repo
SASKI_ROOT=${SASKI_ROOT:-$ROOT/../saski}
if [ ! -d "$SASKI_ROOT/kernel" ]; then
    echo "ERROR: SASKI_ROOT/kernel not found at $SASKI_ROOT/kernel" >&2
    echo "Set SASKI_ROOT to the saski repository root (contains kernel/ with cap.h, schema.h, capnp.h)" >&2
    exit 1
fi

# Architecture-specific configuration
#
# ARCH_EXTRA_OBJS: job_exec_init is implemented once per architecture
# (job_exec_x86_64.c / job_exec_aarch64.c), so exactly ONE of those
# objects may appear on the link line -- both at once would be a
# duplicate symbol. Each arch block below sets it.
ARCH_EXTRA_OBJS=""
case "$ARCH" in
    x86_64)
        TARGET=x86_64-apple-darwin
        CFLAGS="-target $TARGET -std=c11 -ffreestanding -fno-stack-protector -mno-red-zone -mno-sse -mno-mmx -Wall -Wextra -I$ROOT/coreinit -I$SASKI_ROOT/kernel -I$ROOT/include -O1 -g"
        
        # x86_64 source files
        SOURCES="
            coreinit/coreinit.c
            coreinit/serial_x86_64.c
            coreinit/pci_x86_64.c
            coreinit/ata_x86_64.c
            coreinit/irq_x86_64.c
            coreinit/acpi_x86_64.c
            coreinit/smp_x86_64.c
            coreinit/mutex_mgr.c
            coreinit/mem_mgr.c
            coreinit/lapic_timer_x86_64.c
            coreinit/scheduler.c
            coreinit/cluster_scheduler.c
            coreinit/proxy.c
            coreinit/kinv.c
            coreinit/pagefault_x86_64.c
            coreinit/job_memory.c
            coreinit/job_exec.c
            coreinit/job_exec_x86_64.c
            coreinit/cross_core_signal_x86_64.c
            coreinit/arch_mmu_x86_64.c
            coreinit/arch_cpu_x86_64.c
        "
        ASM_SOURCES="coreinit/ap_trampoline_x86_64.S coreinit/job_exec_switch_x86_64.S"
        LD_ARCH=x86_64
        PAGEZERO=0x1800000
        # The AP trampoline object is deliberately NOT listed here: ld64
        # cannot link its 16-bit real-mode relocations, and smp_x86_64.c
        # consumes the pre-generated flat binary in
        # ap_trampoline_blob.h instead -- it is compiled, never linked.
        ARCH_EXTRA_OBJS="$BUILD/coreinit_job_exec_x86_64.o"
        ;;
    aarch64)
        TARGET=aarch64-apple-darwin
        CFLAGS="-target $TARGET -mcpu=generic -march=armv8-a -mgeneral-regs-only -std=c11 -ffreestanding -fno-stack-protector -Wall -Wextra -I$ROOT/coreinit -I$SASKI_ROOT/kernel -I$ROOT/include -O1 -g"
        
        # aarch64 source files (complete at the link level; ARM boot path
        # is future work)
        SOURCES="
            coreinit/coreinit.c
            coreinit/serial_aarch64.c
            coreinit/topology_aarch64.c
            coreinit/pci_aarch64.c
            coreinit/ata_aarch64.c
            coreinit/irq_aarch64.c
            coreinit/timer_aarch64.c
            coreinit/pagefault_aarch64.c
            lib/fdt.c
            coreinit/mutex_mgr.c
            coreinit/mem_mgr.c
            coreinit/scheduler.c
            coreinit/cluster_scheduler.c
            coreinit/proxy.c
            coreinit/kinv.c
              coreinit/job_memory.c
              coreinit/job_exec.c
              coreinit/job_exec_aarch64.c
              coreinit/arch_cpu_aarch64.c
              coreinit/cross_core_signal_aarch64.c
              coreinit/smp_aarch64.c
              coreinit/arch_mmu_aarch64.c
        "
        ASM_SOURCES="coreinit/job_exec_switch_aarch64.S"
        LD_ARCH=arm64
        # 0x40200000 is 2 MiB into QEMU virt's RAM (base 0x40000000) --
        # the x86 convention's 24 MiB is below virt's RAM base and would
        # be an unloadable image. coreinit still declares its own load
        # address and the kernel honors it via target_base=0 (same
        # convention, only the number changes).
        #
        # ADDRESS MARGIN, stated rather than left as an accident of two
        # independent choices: the aarch64 kernel links at 0x40080000, so
        # this load address sits 1.5 MiB (0x180000) above the kernel's
        # base. The kernel image is ~190 KiB today, leaving roughly 1.3 MiB
        # of slack. That is comfortable but not enforced by anything -- a
        # kernel that grows past ~1.5 MiB, or a coreinit image that grows
        # down, would collide silently. If either outgrows its space, move
        # coreinit up (0x40400000 is 4 MiB in) rather than shrinking the
        # kernel: coreinit declares its own address, so this is a one-line
        # change on this side with no kernel edit.
        PAGEZERO=0x40200000
        # The FDT object belongs here rather than on the shared link
        # line: lib/fdt.c is only in this arch's SOURCES, so the object
        # doesn't exist for an x86_64 build. No trampoline object either
        # -- PSCI CPU_ON replaces it (the AP entry contract is
        # documented in smp_aarch64.c).
        ARCH_EXTRA_OBJS="$BUILD/coreinit_job_exec_aarch64.o $BUILD/lib_fdt.o"

        # The aarch64 build is complete at the link level. The ARM BOOT
        # path is separate future work (G4c): it needs the saski S4 DTB
        # handoff and an ARM kernel_boot_ap, not a build gap --
        # ARCH=aarch64 produces a valid, unbootable-by-design image until
        # that lands.
        ;;
    *)
        echo "ERROR: Unknown ARCH='$ARCH'. Supported: x86_64, aarch64" >&2
        exit 1
        ;;
esac

# Compile C sources
for src in $SOURCES; do
    base=$(basename "$src" .c)
    case "$base" in
        cross_core_signal_x86_64|cross_core_signal_aarch64)
            obj="$BUILD/coreinit_cross_core_signal.o"
            ;;
        arch_mmu_x86_64|arch_mmu_aarch64)
            obj="$BUILD/coreinit_arch_mmu.o"
            ;;
        pagefault_x86_64|pagefault_aarch64)
            obj="$BUILD/coreinit_pagefault.o"
            ;;
        arch_cpu_x86_64|arch_cpu_aarch64)
            obj="$BUILD/coreinit_arch_cpu.o"
            ;;
        serial_x86_64|serial_aarch64)
            obj="$BUILD/coreinit_serial.o"
            ;;
        topology_x86_64|topology_aarch64)
            obj="$BUILD/coreinit_topology.o"
            ;;
          irq_x86_64|irq_aarch64)
            obj="$BUILD/coreinit_irq.o"
            ;;
            smp_x86_64|smp_aarch64)
            obj="$BUILD/coreinit_smp.o"
            ;;
        lapic_timer_x86_64|timer_aarch64)
            obj="$BUILD/coreinit_lapic_timer.o"
            ;;
        pci_x86_64|pci_aarch64)
            obj="$BUILD/coreinit_pci.o"
            ;;
        ata_x86_64|ata_aarch64)
            obj="$BUILD/coreinit_ata.o"
            ;;
        acpi_x86_64)
            # x86's topology backend IS the ACPI one: it emits the same
            # coreinit_topology.o aarch64 gets from topology_aarch64.c,
            # so the shared link line stays arch-neutral.
            obj="$BUILD/coreinit_topology.o"
            ;;
        coreinit)
            obj="$BUILD/coreinit_main.o"
            ;;
          fdt)
            obj="$BUILD/lib_fdt.o"
            ;;
        *)
            obj="$BUILD/coreinit_${base}.o"
            ;;
    esac
    "$CC" $CFLAGS -c "$ROOT/$src" -o "$obj"
done

# Compile assembly sources
for src in $ASM_SOURCES; do
    base=$(basename "$src" .S)
    case "$base" in
          ap_trampoline_x86_64)
              obj="$BUILD/coreinit_ap_trampoline.o"
            ;;
        job_exec_switch_x86_64|job_exec_switch_aarch64)
            obj="$BUILD/coreinit_job_exec_switch.o"
            ;;
        *)
            obj="$BUILD/coreinit_${base}.o"
            ;;
    esac
    "$CC" -target $TARGET -c "$ROOT/$src" -o "$obj"
done

# Link
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
"$LD64" -arch "$LD_ARCH" -platform_version macos 10.12 10.12 \
    -e _coreinit_main -pagezero_size "$PAGEZERO" \
    -o "$BUILD/coreinit.macho" \
    "$BUILD/coreinit_main.o" "$BUILD/coreinit_serial.o" "$BUILD/coreinit_pci.o" \
    "$BUILD/coreinit_ata.o" "$BUILD/coreinit_irq.o" "$BUILD/coreinit_topology.o" \
    "$BUILD/coreinit_smp.o" \
    "$BUILD/coreinit_mutex_mgr.o" "$BUILD/coreinit_mem_mgr.o" \
    "$BUILD/coreinit_lapic_timer.o" "$BUILD/coreinit_scheduler.o" \
    "$BUILD/coreinit_cluster_scheduler.o" "$BUILD/coreinit_proxy.o" \
    "$BUILD/coreinit_kinv.o" \
    "$BUILD/coreinit_pagefault.o" "$BUILD/coreinit_job_memory.o" \
    "$BUILD/coreinit_job_exec.o" "$BUILD/coreinit_job_exec_switch.o" \
    "$BUILD/coreinit_cross_core_signal.o" "$BUILD/coreinit_arch_mmu.o" \
    "$BUILD/coreinit_arch_cpu.o" $ARCH_EXTRA_OBJS

echo "built $BUILD/coreinit.macho"
file "$BUILD/coreinit.macho"
