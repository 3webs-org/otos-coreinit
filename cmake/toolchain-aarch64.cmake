# Freestanding aarch64 Mach-O toolchain. Selected via:
#   cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64.cmake
#
# Only the compiler side lives here. The final link still goes through the
# clang driver (see the top-level CMakeLists.txt's target_link_options),
# with --ld-path selecting ld64.lld (LD64) over the system default, since
# this is a non-hosted Mach-O with no libSystem/CRT.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

if(NOT CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER clang-18)
endif()
if(NOT CMAKE_ASM_COMPILER)
    set(CMAKE_ASM_COMPILER "${CMAKE_C_COMPILER}")
endif()

set(CMAKE_C_COMPILER_TARGET   aarch64-apple-darwin)
set(CMAKE_ASM_COMPILER_TARGET aarch64-apple-darwin)

# Required for a correct freestanding aarch64 image -- ABI facts, not
# optimization choices, so they live here rather than being left to the
# caller the way CMAKE_BUILD_TYPE is.
#
# -fno-pic, NOT -fpie, deliberately -- matching saski's own aarch64
# toolchain's identical choice, for the identical reason: an AP runs a raw
# byte-for-byte copy of this image at an address assigned at runtime, with
# no dyld present to rebase anything. On aarch64, clang's -fpie codegen
# routes many global/function references through a real GOT with
# BSP-absolute entries that only a dynamic linker would fix up -- unlike
# x86_64, where -fpie's RIP-relative codegen needs no such indirection for
# a fully static, single-binary image. -fno-pic emits PC-relative
# ADRP/ADD references to local symbols instead, which stay correct at any
# load address with no relocation step at all.
set(CMAKE_C_FLAGS_INIT
    "-mcpu=generic -march=armv8-a -mgeneral-regs-only -std=c11 -ffreestanding -fno-stack-protector -fno-pic -Wall -Wextra")
