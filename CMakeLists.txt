# coreinit build.
#
# Usage:
#   cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-x86_64.cmake -DLD64=/path/to/ld64.lld
#   cmake --build build
#
# (toolchain-aarch64.cmake for the other architecture.) CMAKE_SYSTEM_PROCESSOR,
# set by the toolchain file, selects which architecture's sources below get
# built -- switching it on an existing build/ directory isn't supported by
# CMake, so switching architectures means a fresh build/ (rm -rf build).
#
# No CMAKE_BUILD_TYPE is set by this file: see the note below.
#
# ARCHITECTURE CONVENTION
# ------------------------
# Arch-specific implementation lives in <header-stem>_<arch>.c files (e.g.,
# arch_mmu_x86_64.c, cross_core_signal_x86_64.c). Headers stay arch-neutral
# and define the seam (e.g., arch_mmu.h, cross_core_signal.h). Adding a new
# architecture means adding a file list and a toolchain file, not adding
# #ifdef ladders inside shared files or a case arm for an object's name --
# CMake's normal executable targets track each source's own object
# themselves, so nothing here renames or re-lists anything.

cmake_minimum_required(VERSION 3.21)
project(coreinit C ASM)

if(NOT CMAKE_BUILD_TYPE)
    message(STATUS "note: no CMAKE_BUILD_TYPE is set, so this build applies no optimization level or debug info.")
    message(STATUS "      -DCMAKE_BUILD_TYPE=MinSizeRel is a reasonable default for this freestanding image (-Os).")
    message(STATUS "      -DCMAKE_BUILD_TYPE=Debug adds -g instead.")
endif()

# Contract headers from the kernel repo.
if(DEFINED ENV{SASKI_ROOT})
    set(SASKI_ROOT_DEFAULT "$ENV{SASKI_ROOT}")
else()
    set(SASKI_ROOT_DEFAULT "${CMAKE_SOURCE_DIR}/../saski")
endif()
set(SASKI_ROOT "${SASKI_ROOT_DEFAULT}" CACHE PATH
    "Root of the saski repository (contains kernel/ with cap.h, schema.h, capnp.h)")
if(NOT EXISTS "${SASKI_ROOT}/kernel")
    message(FATAL_ERROR
        "SASKI_ROOT/kernel not found at ${SASKI_ROOT}/kernel\n"
        "Set -DSASKI_ROOT=... to the saski repository root (contains kernel/ with cap.h, schema.h, capnp.h)")
endif()

set(LD64 "ld64.lld" CACHE STRING "Path to ld64.lld (or a compatible Mach-O linker)")
set(PYTHON3 "python3" CACHE STRING "Python 3 interpreter used for build-time code generation")

set(COMMON_INCLUDES
    "${CMAKE_SOURCE_DIR}/coreinit"
    "${SASKI_ROOT}/kernel"
    "${CMAKE_SOURCE_DIR}/include"
)

# Architecture-specific configuration: which files to build, and the few
# facts (link arch, load address, which securinit wrapper) that genuinely
# differ by platform.
if(CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
    set(LD_ARCH x86_64)
    # 24 MiB, matching include/smp_layout.h's own BSP_COREINIT_LOAD_BASE:
    # coreinit declares its OWN preferred load address here, via its own
    # build, rather than the kernel deciding one for it at runtime. See the
    # final link step below for the fuller rationale.
    set(PAGEZERO 0x1800000)
    set(WRAPPER_PREFIX x86_wrapper)
    set(WRAPPER_ASM securinit/x86_wrapper_x86_64.S)
    set(CORE_SOURCES
        coreinit/coreinit.c
        coreinit/serial_x86_64.c
        coreinit/pci_x86_64.c
        coreinit/ata_x86_64.c
        coreinit/irq_x86_64.c
        coreinit/acpi_x86_64.c          # x86's topology backend; same interface aarch64 gets from topology_aarch64.c
        coreinit/smp_x86_64.c
        coreinit/mutex_mgr.c
        coreinit/mem_mgr.c
        coreinit/lapic_timer_x86_64.c
        coreinit/scheduler.c
        coreinit/cluster_scheduler.c
        coreinit/proxy.c
        coreinit/kinv.c
        coreinit/stage_loader.c
        coreinit/pagefault_x86_64.c
        coreinit/job_memory.c
        coreinit/job_exec.c
        coreinit/job_exec_x86_64.c
        coreinit/cross_core_signal_x86_64.c
        coreinit/arch_mmu_x86_64.c
        coreinit/arch_cpu_x86_64.c
        coreinit/job_exec_switch_x86_64.S
    )
    # The AP trampoline is deliberately NOT part of CORE_SOURCES / the final
    # link: ld64 cannot link its 16-bit real-mode relocations, and
    # smp_x86_64.c consumes the pre-generated flat binary in
    # ap_trampoline_blob.h instead. It's still compiled below, as its own
    # object library that nothing links against, purely so a syntax error
    # in it fails the build rather than going unnoticed.
    set(NOLINK_SOURCES coreinit/ap_trampoline_x86_64.S)
elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
    set(LD_ARCH arm64)
    # 0x40200000 is 2 MiB into QEMU virt's RAM (base 0x40000000) -- the
    # x86 convention's 24 MiB is below virt's RAM base and would be an
    # unloadable image. coreinit still declares its own load address and
    # the kernel honors it via target_base=0 (same convention, only the
    # number changes).
    #
    # ADDRESS MARGIN, stated rather than left as an accident of two
    # independent choices: the aarch64 kernel links at 0x40080000, so this
    # load address sits 1.5 MiB (0x180000) above the kernel's base. The
    # kernel image is ~190 KiB today, leaving roughly 1.3 MiB of slack.
    # That is comfortable but not enforced by anything -- a kernel that
    # grows past ~1.5 MiB, or a coreinit image that grows down, would
    # collide silently. If either outgrows its space, move coreinit up
    # (0x40400000 is 4 MiB in) rather than shrinking the kernel: coreinit
    # declares its own address, so this is a one-line change on this side
    # with no kernel edit.
    set(PAGEZERO 0x40200000)
    set(WRAPPER_PREFIX arm_wrapper)
    set(WRAPPER_ASM securinit/arm_wrapper_aarch64.S)
    set(CORE_SOURCES
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
        coreinit/stage_loader.c
        coreinit/job_memory.c
        coreinit/job_exec.c
        coreinit/job_exec_aarch64.c
        coreinit/arch_cpu_aarch64.c
        coreinit/cross_core_signal_aarch64.c
        coreinit/smp_aarch64.c
        coreinit/arch_mmu_aarch64.c
        coreinit/job_exec_switch_aarch64.S
        coreinit/irq_vectors_aarch64.S
    )
    # The aarch64 build is complete at the link level. The ARM BOOT path is
    # separate future work (G4c): it needs the saski S4 DTB handoff and an
    # ARM kernel_boot_ap, not a build gap -- this produces a valid,
    # unbootable-by-design image until that lands.
    set(NOLINK_SOURCES "")
else()
    message(FATAL_ERROR
        "Unknown CMAKE_SYSTEM_PROCESSOR='${CMAKE_SYSTEM_PROCESSOR}'. Supported: x86_64, aarch64\n"
        "Select one with -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-<arch>.cmake")
endif()

# Cap'n Proto source is compiled on the host for every build, regardless of
# the kernel's EMBEDDED machine-description choice. The output contains
# only const index-based C tables -- no schema text or runtime parser.
set(SCHEMA_GEN "${CMAKE_SOURCE_DIR}/tools/gen-capnp-schemas.py")
add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/generated_schemas.c" "${CMAKE_BINARY_DIR}/generated_schemas.h"
    COMMAND "${PYTHON3}" "${SCHEMA_GEN}" "${CMAKE_SOURCE_DIR}/schemas/coreinit.capnp" "${CMAKE_BINARY_DIR}"
    DEPENDS "${SCHEMA_GEN}" "${CMAKE_SOURCE_DIR}/schemas/coreinit.capnp" "${CMAKE_SOURCE_DIR}/schemas/abi-baseline.json"
    COMMENT "Generating coreinit capnp schema tables"
)

# Build stage 2 (securinit) independently. Its object schemas live in its
# OWN Mach-O; only the finished image bytes are embedded in coreinit below.
set(SECURINIT_BUILD "${CMAKE_BINARY_DIR}/securinit-generated")
add_custom_command(
    OUTPUT "${SECURINIT_BUILD}/generated_schemas.c" "${SECURINIT_BUILD}/generated_schemas.h"
    COMMAND "${PYTHON3}" "${SCHEMA_GEN}" "${CMAKE_SOURCE_DIR}/schemas/securinit.capnp" "${SECURINIT_BUILD}"
    DEPENDS "${SCHEMA_GEN}" "${CMAKE_SOURCE_DIR}/schemas/securinit.capnp" "${CMAKE_SOURCE_DIR}/schemas/securinit-abi-baseline.json"
    COMMENT "Generating securinit capnp schema tables"
)

add_executable(securinit
    securinit/securinit.c
    securinit/user_ipc.c
    "${SECURINIT_BUILD}/generated_schemas.c"
    "securinit/${WRAPPER_PREFIX}.c"
    "${WRAPPER_ASM}"
    lib/freestanding_mem.c
)
target_include_directories(securinit PRIVATE ${COMMON_INCLUDES} "${SECURINIT_BUILD}")
set_target_properties(securinit PROPERTIES SUFFIX ".macho" RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
# -nostdlib: link nothing implicit (no libSystem/CRT) -- this is a
# non-hosted Mach-O with its own entry point, not a normal macOS
# executable. -arch/-platform_version are passed explicitly rather than
# left to the driver's own defaults: clang's Darwin linker-flag synthesis
# assumes an actual macOS SDK/host, which isn't present when cross-linking
# from Linux, so it omits them unless told.
target_link_options(securinit PRIVATE
    --ld-path=${LD64} -nostdlib
    -Wl,-arch,${LD_ARCH} -Wl,-platform_version,macos,10.12,10.12
    -Wl,-pagezero_size,0 -Wl,-e,_securinit_main
)

if(NOLINK_SOURCES)
    # Compiled only -- see the comment above where NOLINK_SOURCES is set.
    add_library(ap_trampoline_objs OBJECT ${NOLINK_SOURCES})
    target_include_directories(ap_trampoline_objs PRIVATE ${COMMON_INCLUDES})
endif()

# -pagezero_size (set per architecture above) is coreinit declaring its OWN
# preferred load address here, via its own build, rather than the kernel
# deciding one for it at runtime. This is a deliberate principle, not an
# implementation detail -- the kernel should never itself choose where to
# place something; it should only ever act on a location it was actually
# given (see kernel.c's own kernel_boot, which now calls macho_load with
# target_base=0 for coreinit specifically because of this: 0 means
# "wherever your own vmaddr already says", not "no relocation" -- coreinit's
# vmaddr now IS this address, by this build's own declaration, so
# target_base=0 correctly lands it there without the kernel ever consulting
# a load-base constant itself). An AP's own copy is a genuinely different
# case, still legitimately relocated by a target_base the kernel receives
# from its actual caller (coreinit's own smp_bring_up_all, via the AP
# trampoline's region_base parameter) -- externally supplied, not
# self-decided, which is exactly the distinction this principle is about.
add_executable(coreinit
    ${CORE_SOURCES}
    "${CMAKE_BINARY_DIR}/generated_schemas.c"
    coreinit/securinit_blob.S
    lib/freestanding_mem.c
)
target_include_directories(coreinit PRIVATE ${COMMON_INCLUDES} "${CMAKE_BINARY_DIR}")
set_target_properties(coreinit PROPERTIES SUFFIX ".macho" RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
target_link_options(coreinit PRIVATE
    --ld-path=${LD64} -nostdlib
    -Wl,-arch,${LD_ARCH} -Wl,-platform_version,macos,10.12,10.12
    -Wl,-pagezero_size,${PAGEZERO} -Wl,-e,_coreinit_main
)
set_source_files_properties(coreinit/securinit_blob.S PROPERTIES
    COMPILE_DEFINITIONS "SECURINIT_MACHO_PATH=\"$<TARGET_FILE:securinit>\""
)
add_dependencies(coreinit securinit)
add_custom_command(TARGET coreinit POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E echo "built $<TARGET_FILE:coreinit>"
    COMMAND file "$<TARGET_FILE:coreinit>"
    VERBATIM
)
